# Summarize only retained boxes. BEST_RAW is below-threshold diagnostic data when kept=0.
$1 == "DECODE" {
    headers++
    for (i = 2; i <= NF; i++) {
        if ($i ~ /^scanned=[0-9]+$/) scanned = substr($i, 9)
        if ($i ~ /^kept=[0-9]+$/) { kept = substr($i, 6); has_kept = 1 }
    }
}
$1 == "DETECTION" {
    if ($2 == "class=0") inserted++
    else if ($2 == "class=1") exposed++
    else invalid = 1
    count++
}
$0 == "PASS native one-frame inference" { pass++ }
END {
    if (invalid || headers != 1 || !has_kept || scanned != 25200 || kept+0 != count+0 || pass != 1) {
        print "incomplete decoder result" > "/dev/stderr"
        exit 1
    }
    if (!count) { state = "unknown"; reason = "no_detection" }
    else if (inserted && exposed) { state = "unknown"; reason = "conflicting_classes" }
    else { state = inserted ? "plug_inserted" : "pins_exposed"; reason = "detected" }
    printf "INSPECT %s detections=%d state=%s reason=%s\n", ids, count, state, reason
}
