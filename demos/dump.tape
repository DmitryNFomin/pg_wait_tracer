Output demos/dump.gif
Set FontSize 14
Set Width 1200
Set Height 900
Set Theme "Dracula"
Set Shell "bash"
Set TypingSpeed 25ms

Hide
Type "export PS1='$ ' && clear"
Enter
Show

Type "# pg_wait_tracer — offline replay of a 60s pgbench trace"
Enter
Sleep 1500ms
Type "# Mixed waits: pgbench, table locks, and heavy reads"
Enter
Sleep 1500ms
Type `ls "$TRACE_DIR"`
Enter
Sleep 2500ms

Type `./pgwt-server --dump "$TRACE_DIR" 2>/dev/null | head -35`
Enter
Sleep 10s

Type "clear"
Enter
Sleep 300ms

Type "# Top sessions + top queries (by query_id)"
Enter
Sleep 1200ms
Type `./pgwt-server --dump "$TRACE_DIR" 2>/dev/null | sed -n '/=== Top Sessions ===/,$p'`
Enter
Sleep 10s
