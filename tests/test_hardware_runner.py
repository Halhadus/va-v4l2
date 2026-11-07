import importlib.util
from pathlib import Path
import unittest
import io
import tarfile
import tempfile
from unittest.mock import patch
from encoder_pool import x265_pool

spec = importlib.util.spec_from_file_location("hardware", Path(__file__).parents[1] / "tools/hardware.py")
hardware = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hardware)


class Hardware(unittest.TestCase):
    def test_encoder_pool_uses_a_bare_count_for_a_single_node(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); node = root / 'node0'; node.mkdir(); (node / 'cpulist').write_text('0-3')
            with patch('encoder_pool.os.sched_getaffinity', return_value={0, 1}):
                self.assertEqual(x265_pool(node_root=root), '1')
                self.assertEqual(x265_pool(None, root), '2')

    def test_encoder_pool_uses_available_cpus_without_assuming_node_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for node, cpus in ((0, '0-1'), (1, ''), (2, '2-3,8'), (7, '9-10')):
                path = root / f'node{node}'; path.mkdir(); (path / 'cpulist').write_text(cpus)
            for allowed, expected, automatic in (({0}, '1,-', '1,-'), ({2, 3, 8}, '-,-,1,-', '-,-,3,-'),
                ({9}, '-,-,-,-,-,-,-,1,-', '-,-,-,-,-,-,-,1,-')):
                with self.subTest(allowed=allowed), patch('encoder_pool.os.sched_getaffinity', return_value=allowed):
                    self.assertEqual(x265_pool(node_root=root), expected)
                    self.assertEqual(x265_pool(None, root), automatic)
            with patch('encoder_pool.os.sched_getaffinity', return_value={99}):
                self.assertEqual(x265_pool(node_root=root), '1')
                self.assertEqual(x265_pool(node_root=root / 'missing'), '1')
                self.assertEqual(x265_pool(None, root / 'missing'), '*')
            with patch('encoder_pool.os.sched_getaffinity', side_effect=OSError):
                self.assertEqual(x265_pool(node_root=root), '1')
            with patch('encoder_pool.Path.glob', side_effect=OSError):
                self.assertEqual(x265_pool(node_root=root), '1')

    def test_acceptance_groups_follow_interfaces_without_board_identity(self):
        profiles = ["VAProfileH264ConstrainedBaseline", "VAProfileH264Main", "VAProfileH264High", "VAProfileH264High10", "VAProfileH264High422",
            "VAProfileHEVCMain", "VAProfileHEVCMain10", "VAProfileVP8Version0_3", "VAProfileMPEG2Simple", "VAProfileMPEG2Main"]
        caps = {"profiles": [{"profile": p, "entrypoint": "VAEntrypointVLD"} for p in profiles] + [
            {"profile": "VAProfileJPEGBaseline", "entrypoint": "VAEntrypointEncPicture"}, {"profile": "VAProfileNone", "entrypoint": "VAEntrypointVideoProc"}]}
        inventory = {"nodes": [{"roles": ["raw M2M candidate"], "sections": [{"stdout": "'NV12' 'NV21' 'YU12' 'NV16' 'BA24' 'AR24' 'XR24' 'RGBP' rotate horizontal_flip vertical_flip"}]}]}
        self.assertTrue(all(hardware.acceptance_plan(caps, inventory).values()))
        caps["profiles"] = [entry for entry in caps["profiles"] if entry["profile"] != "VAProfileHEVCMain10"]
        plan = hardware.acceptance_plan(caps, inventory)
        self.assertFalse(plan["matrix"] or plan["imported"] or plan["direct-capture"])
        self.assertTrue(plan["features"] and plan["renderer"] and plan["transport"])
        plan = hardware.acceptance_plan(caps, {})
        self.assertFalse(plan["features"] or plan["transport"])
        self.assertTrue(plan["renderer"])

    def test_acceptance_report_keeps_nested_failures_and_skips(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hardware.tar.gz"
            with tarfile.open(path, "w:gz") as archive:
                for name, text in [("run/results.tsv", "NAME\tSTATUS\tEXIT_CODE\nmatrix\tFAIL\t1\n"),
                    ("run/matrix/results.tsv", "RESULT\tCASE\nPASS\thevc-main\nFAIL\thevc10\nSKIP\tav1\n"),
                    ("run/renderer.tsv", "PASS\tNV12\nSKIP\tY210\nCOMPLETE\t1\n"),
                    ("run/transport.tsv", "STATUS\tNODE\tFORMAT\nPASS\t/dev/video0\tNV12\nSKIP\t/dev/video0\tY210\n"),
                    ("run/trace.tsv", "TIME\tNODE\n123\t/dev/video0\n")]:
                    data = text.encode(); member = tarfile.TarInfo(name); member.size = len(data); archive.addfile(member, io.BytesIO(data))
            cases = hardware.acceptance_cases(path)
            self.assertEqual([case["status"] for case in cases], ["FAIL", "PASS", "FAIL", "SKIP", "PASS", "SKIP", "PASS", "SKIP"])
            self.assertEqual(len({case["name"] for case in cases}), 8)

    def test_profiles_are_independent_of_board_and_each_other(self):
        profiles = {"profiles": [{"profile": "VAProfileHEVCMain", "entrypoint": "VAEntrypointVLD"},
            {"profile": "VAProfileH264High", "entrypoint": "VAEntrypointEncSlice"}]}
        self.assertEqual(hardware.select_codecs(profiles), ["hevc"])
        self.assertEqual(hardware.select_codecs({"profiles": []}), [])
        poc1 = {"profiles": [{"profile": p, "entrypoint": "VAEntrypointVLD"} for p in ("VAProfileH264ConstrainedBaseline", "VAProfileH264Main")]}
        self.assertEqual(hardware.select_codecs(poc1), ["h264-poc1-cavlc", "h264-poc1-cabac"])

    def test_single_decoder_gets_imported_tests_without_processing(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); output = root / "output"; build = root / "build"; output.mkdir(); build.mkdir()
            for name in ("va_capabilities", "prime_surface_interpose.so", "decode_surface_interpose.so"): (build / name).touch()
            calls = []
            def run(name, argv, env):
                calls.append((name, argv, env))
                if name == "va-capabilities": (output / "va-capabilities.log").write_text(json.dumps({"profiles": [{"profile": "VAProfileHEVCMain", "entrypoint": "VAEntrypointVLD"}]}))
                if name.endswith("-reference"): Path(argv[-1]).write_bytes(self.frames() * 2)
                if "-hwaccel_output_format" in argv:
                    Path(argv[-1]).write_bytes(self.frames() * 2)
                    Path(env["VAV4L2_DECODE_TRACE"]).write_text("STAGE=submit-done\tSTATUS=0\n" * (hardware.FRAMES * 2))
                    if "VAV4L2_EXPORT_TRACE_DIR" in env: (Path(env["VAV4L2_EXPORT_TRACE_DIR"]) / "export-1.tsv").write_text("".join(f"HOLD\t{i}\n" for i in range(1, hardware.FRAMES * 2 + 1)) + f"COMPLETE\t{hardware.FRAMES * 2}\n")
                    if "VAV4L2_PRIME_TRACE_DIR" in env: (Path(env["VAV4L2_PRIME_TRACE_DIR"]) / "prime-1.tsv").write_text("1\t320\t192\t8\t0\n")
                return {"name": name, "status": "PASS", "exit_code": 0, "duration_s": 0}
            with patch.object(hardware.shutil, "which", return_value="ffmpeg"), patch.object(hardware, "run_processing", return_value=[]):
                cases = hardware.run_hardware(run, output, build, "/dev/dri/renderD128", repeat=2)
            results = {case["name"]: case for case in cases}
            for name in ("decode-hevc", "decode-hevc-import-padded", "decode-hevc-import-canonical", "decode-hevc-export-default", "decode-hevc-export-gbm"):
                self.assertEqual(results[name]["status"], "PASS")
                self.assertEqual(results[name]["completions"], {"stateful": 0, "stateless": hardware.FRAMES * 2})
            self.assertTrue(all("-stream_loop" in argv and argv[argv.index("-stream_loop") + 1] == "1" for name, argv, _ in calls if name == "decode-hevc-reference" or "-hwaccel_output_format" in argv))
            imports = [env for name, _, env in calls if name.startswith("decode-hevc-import-")]
            self.assertEqual(len(imports), 2)
            self.assertTrue(all(env["VAV4L2_CAPTURE_DMABUF"] == "0" for env in imports))
            self.assertEqual(results["acceptance-imported"]["status"], "SKIP")

    def test_repeated_comparison_streams_are_bounded_and_exact(self):
        frames = self.frames()
        hardware.compare_streams(io.BytesIO(frames * 3), io.BytesIO(frames * 3), "hevc", hardware.FRAMES * 3)
        for actual in (frames * 3 + b"x", (frames * 3)[:-1], frames * 2 + b"x" * len(frames)):
            with self.assertRaises(ValueError): hardware.compare_streams(io.BytesIO(actual), io.BytesIO(frames * 3), "hevc", hardware.FRAMES * 3)
        class Bounded(io.BytesIO):
            def read(self, size=-1):
                if size < 0 or size > hardware.WIDTH * hardware.HEIGHT * 3 // 2: raise AssertionError("unbounded pixel read")
                return super().read(size)
        hardware.compare_streams(Bounded(frames * 3), Bounded(frames * 3), "hevc", hardware.FRAMES * 3)

    def test_export_trace_cannot_hide_truncation_or_missing_checks(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "export.tsv"
            path.write_text("HOLD\t1\nHOLD\t2\nCOMPLETE\t2\n")
            self.assertEqual(hardware.validate_exports([path], 2), 2)
            for data in ("", "HOLD\t1\n", "HOLD\t1\nCOMPLETE\t2\n", "HOLD\t1\nHOLD\t1\nCOMPLETE\t2\n"):
                path.write_text(data)
                with self.assertRaises(ValueError): hardware.validate_exports([path], 1)
            with self.assertRaises(ValueError): hardware.validate_exports([], 1)

    def frames(self):
        n = hardware.WIDTH * hardware.HEIGHT * 3 // 2
        return b"".join(bytes([16 + frame]) * n for frame in range(hardware.FRAMES))

    def test_unchanged_or_truncated_output_is_not_success(self):
        frames = self.frames()
        hardware.compare_pixels(frames, frames, "hevc")
        for actual in (frames[:-1], frames[:len(frames) // hardware.FRAMES] * hardware.FRAMES):
            with self.assertRaises(ValueError): hardware.compare_pixels(actual, frames, "hevc")

    def test_exact_and_idct_comparisons_are_distinct(self):
        frames = self.frames()
        rounded = bytes([frames[0] + 1]) + frames[1:]
        with self.assertRaises(ValueError): hardware.compare_pixels(rounded, frames, "h264")
        hardware.compare_pixels(rounded, frames, "mpeg2")
        corrupt = bytes([frames[0] + 5]) + frames[1:]
        with self.assertRaises(ValueError): hardware.compare_pixels(corrupt, frames, "mpeg2")


if __name__ == "__main__": unittest.main()
