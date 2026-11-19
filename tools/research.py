"""Read-only IDA research agent: a cheap model (OpenRouter) answers one question about EvilWithin.exe using the
IDA Pro MCP server, and writes a short report. It can only READ the IDB (decompile, disassemble, xrefs, strings,
names); nothing is renamed, patched or saved.

Usage:
    set OPENROUTER_API_KEY=...            (never paste the key into a chat or a file in the repo)
    python tools/research.py question.txt report.md [--model deepseek/deepseek-v4.1-flash] [--steps 30]

Needs IDA open with the ida-pro-mcp plugin serving http://127.0.0.1:13337/mcp.
"""
import argparse
import json
import os
import sys
import time
import urllib.request

IDA_URL = os.environ.get("IDA_MCP_URL", "http://127.0.0.1:13337/mcp")
OPENROUTER_URL = "https://openrouter.ai/api/v1/chat/completions"
MAX_TOOL_CHARS = 7000  # per tool result, to keep the model's context (and cost) small

SYSTEM = """You are a reverse engineer researching The Evil Within (2014), Steam build 2708258 (EvilWithin.exe,
idTech 5 based, x64). IDA is open on it with image base 0: addresses are RVAs. You only have READ-ONLY IDA tools.
Many functions are already named and commented: names like idPlayer_SetHealth, idAnimatedEntity_StartSynchroMotion
and repeatable comments starting with "[ewcoop]" were written by us and are reliable. Use them.
Known basics: idPlayer vtable 0x15D4B78 (8-byte slots); player health float at +0x8C4; the global current game
pointer is idGameLocal_current (0x9AB3AF0); entity name char* at entity+8; morpheme anim network at entity+1280;
enemy AI anim request 0x7CC920(ai, index) and float param 0x7CD8C0(ai, index, value).
Work efficiently: decompile the most relevant function first, follow xrefs only when needed, do not decompile
huge unrelated functions. When you have the answer, stop calling tools and reply with the final report:
a concise list of addresses / field offsets, what each is, arguments, and a confidence (high/medium/low) per item,
plus the one concrete thing a modder should hook or write. Under 60 lines. Say clearly what you could not find."""

TOOLS = [
    ("decompile", "Decompile the function containing an address. Returns pseudocode.",
     {"addr": {"type": "string", "description": "address like 0x7BE370 or a function name"}}, ["addr"]),
    ("disasm", "Disassemble a function (first N instructions).",
     {"addr": {"type": "string"}, "max_instructions": {"type": "integer", "description": "default 80"}}, ["addr"]),
    ("xrefs_to", "Cross references to an address or name (who calls / reads it).",
     {"addr": {"type": "string"}}, ["addr"]),
    ("search_strings", "Find strings in the binary matching a regex (case-insensitive). Returns address + text.",
     {"pattern": {"type": "string"}}, ["pattern"]),
    ("lookup", "Look up functions/globals by name substring (e.g. 'Sneak', 'idPlayer_').",
     {"query": {"type": "string"}}, ["query"]),
    ("read_bytes", "Read raw bytes at an address (hex).",
     {"addr": {"type": "string"}, "size": {"type": "integer", "description": "max 256"}}, ["addr", "size"]),
]


class Ida:
    def __init__(self, url):
        self.url, self.session, self.id = url, None, 0
        self._rpc("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                                 "clientInfo": {"name": "ewcoop-research", "version": "1"}})
        self._rpc("notifications/initialized", {}, notify=True)

    def _rpc(self, method, params, notify=False):
        body = {"jsonrpc": "2.0", "method": method, "params": params}
        if not notify:
            self.id += 1
            body["id"] = self.id
        req = urllib.request.Request(self.url, data=json.dumps(body).encode(), method="POST",
                                     headers={"Content-Type": "application/json",
                                              "Accept": "application/json, text/event-stream"})
        if self.session:
            req.add_header("Mcp-Session-Id", self.session)
        with urllib.request.urlopen(req, timeout=180) as r:
            self.session = r.headers.get("Mcp-Session-Id") or self.session
            raw = r.read().decode("utf-8", "replace")
        if notify or not raw.strip():
            return None
        if raw.lstrip().startswith("{"):
            msg = json.loads(raw)
        else:  # server-sent events: take the last data line
            data = [l[5:].strip() for l in raw.splitlines() if l.startswith("data:")]
            msg = json.loads(data[-1]) if data else {}
        if "error" in msg:
            raise RuntimeError(msg["error"])
        return msg.get("result")

    def call(self, name, args):
        res = self._rpc("tools/call", {"name": name, "arguments": args}) or {}
        parts = [c.get("text", "") for c in res.get("content", []) if c.get("type") == "text"]
        return "\n".join(parts) if parts else json.dumps(res)[:MAX_TOOL_CHARS]


ANNOTATE_TOOLS = [
    ("rename_function", "Rename a function you understood. Only works on functions still auto-named (sub_XXXX). "
     "Use clear C++-style names like idPlayer_UpdateSpeedLimit.",
     {"addr": {"type": "string"}, "name": {"type": "string"}}, ["addr", "name"]),
    ("add_comment", "Add a 1-2 line function comment: what it does, arguments, return, key field offsets.",
     {"addr": {"type": "string"}, "text": {"type": "string"}}, ["addr", "text"]),
]
ANNOTATE_PROMPT = """
ANNOTATION MODE: while you research, record what you learn in the IDB so the next person doesn't redo it.
For every function you understood with medium or high confidence: call rename_function (only works on sub_XXXX
names, leave existing names alone) and add_comment (what it does, arguments, return, important offsets).
Do not guess names for functions you only glanced at. Then write the final report as usual."""

AUTO_PREFIXES = ("sub_", "nullsub_", "j_sub_")
CHANGES = []  # (kind, addr, text) recorded for the report


def annotate_tool(ida, name, a):
    if name == "rename_function":
        new = "".join(ch for ch in a["name"] if ch.isalnum() or ch == "_")[:80]
        if not new or new[0].isdigit():
            return "refused: invalid name"
        info = ida.call("lookup_funcs", {"queries": a["addr"]})
        try:
            cur = json.loads(info)[0]["fn"]["name"]
        except Exception:
            return f"refused: no function at {a['addr']}"
        if not cur.startswith(AUTO_PREFIXES):
            return f"refused: already named {cur} (only auto names are renamed)"
        out = ida.call("rename", {"batch": {"func": [{"addr": a["addr"], "name": new}]}})
        CHANGES.append(("renamed", a["addr"], f"{cur} -> {new}"))
        return out
    if name == "add_comment":
        text = a["text"].strip()
        if not text.startswith("[ewcoop]"):
            text = "[ewcoop] " + text
        out = ida.call("append_comments", {"items": [{"addr": a["addr"], "comment": text[:400], "scope": "func",
                                                       "dedupe": True}]})
        CHANGES.append(("comment", a["addr"], text[:120]))
        return out
    return None


def run_tool(ida, name, a):
    """Map our small read-only tool set onto the IDA MCP tools."""
    out = annotate_tool(ida, name, a)
    if out is not None:
        return out
    if name == "decompile":
        return ida.call("decompile", {"addr": a["addr"], "include_addresses": False})
    if name == "disasm":
        return ida.call("disasm", {"addr": a["addr"], "max_instructions": int(a.get("max_instructions") or 80)})
    if name == "xrefs_to":
        return ida.call("xrefs_to", {"addrs": a["addr"], "limit": 60})
    if name == "search_strings":
        return ida.call("find_regex", {"pattern": a["pattern"]})
    if name == "lookup":
        return ida.call("lookup_funcs", {"queries": a["query"]})
    if name == "read_bytes":
        return ida.call("get_bytes", {"regions": [{"addr": a["addr"], "size": min(int(a.get("size") or 64), 256)}]})
    return "unknown tool"


def chat(key, model, messages, tools):
    body = {"model": model, "messages": messages, "tools": tools, "temperature": 0.2}
    req = urllib.request.Request(OPENROUTER_URL, data=json.dumps(body).encode(), method="POST",
                                 headers={"Content-Type": "application/json", "Authorization": "Bearer " + key,
                                          "X-Title": "ida-research"})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=300) as r:
                return json.loads(r.read().decode())
        except urllib.error.HTTPError as e:
            detail = e.read().decode("utf-8", "replace")[:500]
            if e.code < 500 and e.code != 429:  # our request is wrong (model, tools...): retrying won't help
                sys.exit(f"OpenRouter HTTP {e.code}: {detail}")
            if attempt == 2:
                sys.exit(f"OpenRouter HTTP {e.code}: {detail}")
            print(f"  (retry after HTTP {e.code}: {detail[:120]})", flush=True)
            time.sleep(5)
        except Exception as e:  # network hiccups: retry
            if attempt == 2:
                raise
            print(f"  (retry after error: {e})", flush=True)
            time.sleep(5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("question")
    ap.add_argument("report")
    ap.add_argument("--model", default=os.environ.get("RESEARCH_MODEL", "deepseek/deepseek-v4.1-flash"))
    ap.add_argument("--steps", type=int, default=30)
    ap.add_argument("--context", help="text file that replaces the built-in Evil Within context (for other "
                                      "programs: what the binary is, image base, known names/addresses)")
    ap.add_argument("--annotate", action="store_true",
                    help="also let the model rename sub_XXXX functions and add [ewcoop] comments, then save the IDB")
    args = ap.parse_args()
    key = os.environ.get("OPENROUTER_API_KEY")
    if not key:
        sys.exit("OPENROUTER_API_KEY is not set")
    question = open(args.question, encoding="utf-8").read()
    ida = Ida(IDA_URL)
    toolset = TOOLS + (ANNOTATE_TOOLS if args.annotate else [])
    tools = [{"type": "function", "function": {"name": n, "description": d,
              "parameters": {"type": "object", "properties": p, "required": r}}} for n, d, p, r in toolset]
    base = SYSTEM
    if args.context:  # another program: keep the generic working rules, replace the Evil Within facts
        ctx = open(args.context, encoding="utf-8").read()
        base = ("You are a reverse engineer. You only have READ-ONLY IDA tools (unless annotation mode says "
                "otherwise). Work efficiently: decompile the most relevant function first, follow xrefs only when "
                "needed. When you have the answer, stop calling tools and reply with the final report: a concise list "
                "of addresses / field offsets, what each is, arguments, and a confidence (high/medium/low) per item, "
                "plus the one concrete thing to hook or write. Under 60 lines. Say clearly what you could not find."
                "\n\nPROJECT CONTEXT:\n" + ctx)
    system = base + (ANNOTATE_PROMPT if args.annotate else "")
    messages = [{"role": "system", "content": system}, {"role": "user", "content": question}]
    tokens_in = tokens_out = 0
    final = None
    for step in range(args.steps):
        last = step == args.steps - 1
        if last:
            messages.append({"role": "user", "content": "Tool budget used up. Write the final report now."})
        # Keep the context small: tool results older than the last 6 shrink to a short preview (the model
        # already read them and can call again). This cut input tokens a lot on long runs.
        tool_msgs = [m for m in messages if m.get("role") == "tool"]
        for m in tool_msgs[:-6]:
            if len(m["content"]) > 600:
                m["content"] = m["content"][:500] + "\n[... older result trimmed; call the tool again if needed]"
        resp = chat(key, args.model, messages, [] if last else tools)
        u = resp.get("usage") or {}
        tokens_in += u.get("prompt_tokens", 0)
        tokens_out += u.get("completion_tokens", 0)
        msg = resp["choices"][0]["message"]
        calls = msg.get("tool_calls") or []
        messages.append({k: v for k, v in msg.items() if k in ("role", "content", "tool_calls")})
        if not calls:
            final = msg.get("content") or ""
            if not final.strip():  # some replies carry only reasoning: ask once more, plainly
                messages.append({"role": "user", "content": "Write the final report now as plain text."})
                resp = chat(key, args.model, messages, [])
                m2 = resp["choices"][0]["message"]
                final = m2.get("content") or m2.get("reasoning") or msg.get("reasoning") or ""
            break
        for c in calls:
            fn = c["function"]["name"]
            try:
                a = json.loads(c["function"].get("arguments") or "{}")
                out = run_tool(ida, fn, a)
            except Exception as e:
                out = f"error: {e}"
            print(f"  [{step}] {fn} {c['function'].get('arguments', '')[:80]} -> {len(out)} chars", flush=True)
            messages.append({"role": "tool", "tool_call_id": c["id"], "content": out[:MAX_TOOL_CHARS]})
    final = final or "(no final answer)"
    if args.annotate and CHANGES:
        ida.call("idb_save", {})
    with open(args.report, "w", encoding="utf-8") as f:
        f.write(final.strip() + "\n")
        if CHANGES:
            f.write("\n## IDB changes (saved)\n")
            for kind, addr, text in CHANGES:
                f.write(f"- {kind} {addr}: {text}\n")
        f.write(f"\n---\nmodel {args.model}, {tokens_in} tokens in, {tokens_out} out\n")
    print(f"report -> {args.report} ({tokens_in} in / {tokens_out} out tokens)")


if __name__ == "__main__":
    main()
