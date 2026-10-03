#!/usr/bin/env python3
"""Drive the AI screen on a board, without paid model requests.

    python3 tools/ai_screen_test.py /dev/ttyACM0
"""
import sys
import uuid
import json

from board import Board, PROMPT


def main():
    b = Board(sys.argv[1])
    scratch = "/tmp/ai-screen-" + uuid.uuid4().hex[:8]
    opened = False
    try:
        b.send(b"\x15\r")
        b.read_until(PROMPT, 5)
        b.run(f"mkdir -p {scratch}/.config/ai-sessions {scratch}/notes {scratch}/project")
        b.run(f"echo test-only > {scratch}/.config/openrouter")
        metadata = dict(version=1, id="demo", title="Offline saved chat", root=scratch + "/notes",
                        model="test/local", effort="low", mode=0, updated=1, partial=False,
                        reported=True, usage=dict(prompt_tokens=100, completion_tokens=40, cost=0.0123))
        messages = [dict(role="user", content="Earlier question"),
                    dict(role="assistant", content="Offline answer **with Markdown**"),
                    dict(role="status", content="100 in / 40 out | 20 avg tok/s | $0.0123")]
        b.run(f"echo '{json.dumps(metadata)}' > {scratch}/.config/ai-sessions/demo.chat")
        b.run(f"echo '{json.dumps(messages)}' >> {scratch}/.config/ai-sessions/demo.chat")

        def keys(data, expected, menu=False, timeout=8):
            b.send(data)
            raw = b.read_until(rb"\x1b\[23;1H" if menu else rb"\x1b\[\?25h", timeout)
            frame = raw.rsplit(b"\x1b[?25l", 1)[-1]
            for text in expected:
                assert text in frame, (data, text, frame[-3000:])
            return raw

        opened = True
        keys(f"HOME={scratch} ai\r".encode(), [b" AI study ", b"0 tok", b"$0.0000"])
        keys(b"/h\t", [b"/help "])
        keys(b"\r", [b"Tab completes", b"\x1b[23;1H"])
        keys(b"/models\r", [b"study:"])
        keys(b"\x1b[A", [b"/models"])
        keys(b"\x15draft\x1b[A\x1b[B", [b"> \x1b[0mdraft"])
        keys(b"\x1b[D\x7fX", [b"draXt"])
        keys(b"\x03", [b"\x1b[23;1H"])
        keys(b"/model\r", [b"Model /", b"choices; type to filter"], menu=True, timeout=35)
        keys(b"qwen", [b"> qwen"], menu=True)
        keys(b"\x1b", [b" AI study "])
        keys(b"/model test/local\r", [b"study: test/local", b"local"])
        keys(b"/effort\r", [b"Reasoning effort", b"low", b"high"], menu=True)
        keys(b"low\r", [b"effort: low"])
        keys(f"/folder {scratch}/project\r".encode(), [b"working in", b"/project"])
        keys(b"/folder /no-such-ai-folder\r", [b"not a folder"])
        for _ in range(3):
            latest = keys(b"/help\r", [b"Tab completes"])
        latest = latest.rsplit(b"\x1b[?25l", 1)[-1]
        older = keys(b"\x01\x1b[A", [b" AI study ", b"\x1b[23;1H"])
        older = older.rsplit(b"\x1b[?25l", 1)[-1]
        assert older != latest, "Ctrl-A Up did not scroll the AI transcript"
        returned = keys(b"\x01\x1b[B", [b" AI study "])
        assert returned.rsplit(b"\x1b[?25l", 1)[-1] == latest
        keys(b"\x01\x1b[H", [b" AI study ", b"working in", b"/project"])
        returned = keys(b"\x01\x1b[F", [b" AI study "])
        assert returned.rsplit(b"\x1b[?25l", 1)[-1] == latest
        keys(b"\x10", [b"\x1b[22;1H", b"-" * 53, b"\x1b[23;1H"])
        keys(b"\x0e", [b" AI study "])
        keys(b"/web\r", [b" AI web "])
        keys(b"/study\r", [b" AI study "])
        keys(b"/sessions\r", [b"Saved chats", b"Offline saved chat"], menu=True)
        keys(b"\r", [b"Resumed:", b"Offline answer", b"140 tok", b"$0.0123"])
        keys(b"/stats\r", [b"100 input / 40 output", b"$0.0123 reported"])
        keys(b"/new\r", [b"a new conversation", b"0 tok", b"$0.0000"])
        b.send(b"\x1b")
        b.read_until(PROMPT, 5)
        opened = False
        opened = True
        keys(f"HOME={scratch} ai resume\r".encode(), [b"Saved chats", b"Offline saved chat"], menu=True)
        keys(b"\r", [b"Offline answer", b"140 tok", b"Resumed:"])
        b.send(b"\x1b")
        b.read_until(PROMPT, 5)
        opened = False
        out = b.run("echo AI_SCREEN_OK")
        assert "AI_SCREEN_OK" in out, (out, b.last_raw)
        print("AI screen: stats header, model/effort menus, folders, editing, scroll, modes, saved chats and Esc exit passed")
    finally:
        if opened:
            b.send(b"\x03\x15/quit\r")
            b.read_until(PROMPT, 5)
        b.run(f"rm -r {scratch}")
        b.close()


if __name__ == "__main__":
    main()
