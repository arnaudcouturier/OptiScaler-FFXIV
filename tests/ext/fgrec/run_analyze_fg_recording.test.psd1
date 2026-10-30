@{
    Class   = 'cpu'
    Note    = 'FG recorder analyzer on fixture sessions: clean exits 0 with p50 35 and p95 67 on the known fixture plus one switch, the hitch and 3 windows, cut exits 2, rotated exits 0, dropped/malformed/nul/mistype/rotcut exit 2, orphan exits 0, live (a session still held open) exits 3 with its message, asdir (the name is a directory) exits 1 and is never called a session still open, and the sharing-violation predicate counts only 32/33 through the base exception (a wrong code, number or flag fails)'
    Runner  = 'run_analyze_fg_recording.ps1'
    Sources = @()
}
