import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import wave

from setup_official_compare import PREPROCESSING_CASES, PREPROCESSING_FIELDS, check_preprocessing_quality, preprocessing_metric_gate


QUALIFIED37 = {
    0: ([10.531304758241776, 12.15471711147192, .02565655743743539, .01633320356925582, 2.6417498673030506, 4.831545268182779, .9883393024140203, .14517995520087212, 99.06155426345742, .7543015063496038, .26498525480175533, 0],
        [3.5300870532725206, 3.8457107232376275, .05744593045764797, .04010750780011948, 2.014102382170895, 4.8217932448323815, .9879935448912385, .15539896340968945, 98.90411779566364, .8815449819254053, .12217782413941403, 0]),
    1: ([13.488762344013637, 16.608001238578396, .01825263224111716, .01062157835727495, 2.9750560863219233, 4.9037777595294365, .9956205365813672, .08116970941717629, 99.01463458173745, .7922018176357377, .2643016380422597, 0],
        [5.088491957661354, 5.856421388505697, .04801080887506167, .03314103183243653, 2.1654304121090617, 4.8780677475979, .993607338399247, .10393809564259554, 98.85358989262436, .9224256371307726, .26834308265669193, 0]),
    169: ([2.403498640016622, -.6116717961002635, .05876076219872085, .04404408362485132, 1.6454861617979302, 4.300414433645765, .845017759923592, .6727313172584951, 41.826210777024855, 69.73175927123658, 237.86918914921014, 0],
          [-.2990025069950851, -2.992730461433146, .08020703789742978, .06058734761116851, 1.46455173332464, 4.27612663685982, .8495948639599228, .7475036941724458, 48.51653805776715, 57.86123642296279, 759.5738891784308, 0]),
}
SMOOTHING20E = {
    0: [10.576706949898423, 12.124674044659061, .02552279741491716, .0162751091773876, 2.6388057416651893, 4.836091822423773, .989723603775503, .14233626239388464, 99.10468946417697, .7194740196177285, .2663762948631226, 0],
    1: [13.211099270358087, 16.54462391979077, .01884554308327272, .01080625575093074, 2.9697205415678343, 4.903296643185472, .9955502379894285, .08154262696096476, 99.01397704125962, .792733086706362, .26734696594648427, 0],
    169: [2.2314899475746395, -.7651977263901812, .05993601371943221, .04502332755470684, 1.634040338739013, 4.309679215882609, .8501265177060062, .6678200311761014, 46.5599120319215, 61.1544217532367, 317.4895422762134, 0],
}
BAD14E_ROW1 = [5.2707610267329095, 6.00277291903165, .04701382316617315, .03246867395809912, 2.1769777113620425, 4.89627917568932, .9951007125996084, .08780570183996751, 99.0282250102011, .7812220299166793, .28916729391866963, 0]


def rejects(call):
    try:
        call()
    except ValueError:
        return
    raise AssertionError("Invalid preprocessing evidence was accepted")


def binding(file):
    return dict(path=file.name, sha256=hashlib.sha256(file.read_bytes()).hexdigest())


def fixture_reports(directory, current):
    source, obj = directory / "candidate.cpp", directory / "candidate.o"
    source.write_bytes(b"source identity fixture")
    obj.write_bytes(b"object identity fixture")
    wav_path = directory / "input.wav"
    with wave.open(str(wav_path), "wb") as wav:
        wav.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
        wav.writeframes(bytes(960 * 2))
    paths = []
    for index, (group, sample, bitrate) in PREPROCESSING_CASES.items():
        row = dict(row_index=index, scope=dict(group=group, sample=sample, application="voip",
                   bitrate=bitrate, complexity=10, postfilter=0, denoise=False),
                   source=binding(source), object=binding(obj), input=binding(wav_path), reference=binding(wav_path))
        for lane, values in (("baseline", QUALIFIED37[index][0]), ("candidate", current[index])):
            report = directory / f"{lane}-{index}.txt"
            header = (f"perceptual validation bitrate={bitrate} official_bitrate={bitrate} input={wav_path} reference={wav_path} "
                      "official_decoder_complexity=0 complexity=10 channels=1 current_postfilter_requested_level=0 "
                      "current_voice_denoise_requested=0 pcm16=0 frames=1\n")
            lines = [label + " " + " ".join(f"{field}={value:.17g}" for field, value in zip(PREPROCESSING_FIELDS, metrics)) + " packets=1\n"
                     for label, metrics in (("current", values), ("official", QUALIFIED37[index][1]))]
            report.write_text(header + "".join(lines))
            row[lane + "_report"] = binding(report)
            row[lane + "_command"] = ["perceptual_memory_validation", "--input", str(wav_path), "--reference", str(wav_path),
                                       "--application", "voip", "--bitrate", str(bitrate), "--complexity", "10",
                                       "--max-seconds", "6", "--skip-memory", "--current-postfilter", "0"]
        path = directory / f"row{index}.json"
        path.write_text(json.dumps(row))
        paths.append(path)
    return paths


def main():
    metrics = lambda values: dict(zip(PREPROCESSING_FIELDS, values))
    parent, official = map(metrics, QUALIFIED37[1])
    rejected14e = preprocessing_metric_gate(parent, metrics(BAD14E_ROW1), official)
    assert rejected14e["status"] == "NO_GO" and rejected14e["new_OFF_deficits"] == ["celt_highband_error"]
    hbe = rejected14e["comparisons"]["celt_highband_error"]
    assert abs(hbe["parent_signed_OFF_margin"] - .00404144) < 1e-8
    assert abs(hbe["candidate_signed_OFF_margin"] + .0208242112619777) < 1e-12
    for lane in range(3):
        for value in (None, float("nan"), float("inf"), -float("inf")):
            args = [parent.copy(), parent.copy(), official.copy()]
            if value is None:
                del args[lane]["celt_highband_error"]
            else:
                args[lane]["celt_highband_error"] = value
            rejects(lambda: preprocessing_metric_gate(*args))
    with tempfile.TemporaryDirectory(prefix="opuscpp preprocessing ") as temporary:
        directory = pathlib.Path(temporary)
        qualified = fixture_reports(directory, {index: values[0] for index, values in QUALIFIED37.items()})
        assert all(row["status"] == "PASS" for row in check_preprocessing_quality(qualified))
        script = pathlib.Path(__file__).with_name("setup_official_compare.py").resolve()
        command = [sys.executable, str(script), "--preprocessing-results", *map(str, qualified)]
        assert subprocess.run(command, cwd=directory, capture_output=True).returncode == 0
        paths = fixture_reports(directory, SMOOTHING20E)
        decisions = {row["row_index"]: row for row in check_preprocessing_quality(paths)}
        assert decisions[0]["status"] == "NO_GO"
        assert decisions[0]["worsened_existing_OFF_deficits"] == ["celt_highband_error"] and not decisions[0]["new_OFF_deficits"]
        assert abs(decisions[0]["comparisons"]["celt_highband_error"]["margin_change"] + .0013910400613672547) < 1e-12
        assert decisions[1]["status"] == "PASS" and "celt_highband_error" in decisions[1]["reduced_positive_OFF_margins"]
        assert decisions[1]["comparisons"]["celt_highband_error"]["candidate_signed_OFF_margin"] > 0
        assert decisions[169]["status"] == "PASS" and decisions[169]["role"] == "target"
        assert decisions[169]["comparisons"]["celt_quality"]["margin_change"] > 1
        assert decisions[169]["comparisons"]["celt_masked_error"]["margin_change"] > 1
        assert subprocess.run(command, cwd=directory, capture_output=True).returncode == 1
        for index in range(3):
            rejects(lambda: check_preprocessing_quality(paths[:index] + paths[index + 1:]))
        rejects(lambda: check_preprocessing_quality([paths[0]] * 3))
        row = json.loads(paths[1].read_text())
        for key in ("source", "object"):
            different = directory / ("different-" + key)
            different.write_bytes(b"different candidate identity")
            changed = dict(row, **{key: binding(different)})
            paths[1].write_text(json.dumps(changed))
            rejects(lambda: check_preprocessing_quality(paths))
        paths[1].write_text(json.dumps(row))
        report = directory / row["candidate_report"]["path"]
        original = report.read_text()
        for replacement in ("", "celt_highband_error=nan"):
            report.write_text(original.replace(f"celt_highband_error={SMOOTHING20E[1][10]:.17g}", replacement))
            paths[1].write_text(json.dumps(dict(row, candidate_report=binding(report))))
            rejects(lambda: check_preprocessing_quality(paths))
    print(json.dumps(dict(status="PASS", qualified37="PASS", smoothing20e_row0="WORSENED_NEGATIVE_REJECTED",
                         smoothing20e_row1="REDUCED_POSITIVE_ACCEPTED", smoothing20e_row169="MATERIAL_GAIN_PASS",
                         bad14e_row1="NEW_NEGATIVE_REJECTED", mandatory_rows=[0, 1, 169],
                         source_object_mismatch="REJECTED", missing_nonfinite_metrics="REJECTED"), indent=2))


if __name__ == "__main__":
    main()
