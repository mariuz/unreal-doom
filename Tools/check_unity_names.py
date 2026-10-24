"""Unity-build name check: names a .cpp keeps to itself (anonymous namespaces, file-level static) that
another .cpp of the same module also uses. In a unity build both files can land in one generated
file, where the two names collide (a redefinition) or one hides the other (C4456-C4459, errors in
this project). Run from the repository root:

    python Tools/check_unity_names.py

Exits 1 and lists each name with the files that share it; give the private one a file-specific name.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Source")
DECL = re.compile(
    r"^\s*(?:static\s+)?(?:constexpr\s+|const\s+|inline\s+)*"
    r"(?:struct|class|enum(?:\s+class)?)\s+(?P<type>[A-Za-z_]\w*)"
    r"|^\s*(?:static\s+)?(?:constexpr\s+|const\s+|inline\s+)*[\w:<>,\s\*&]+?[\s\*&](?P<name>[A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*(?:=|\(|;|\{)"
)
WORD = re.compile(r"[A-Za-z_]\w*")
# names too generic to mean anything (keywords, macros, engine types picked up by the loose pattern)
IGNORE = {"if", "for", "while", "switch", "return", "else", "do", "case", "new", "delete", "sizeof", "operator", "TEXT", "check",
          "true", "false", "nullptr", "const"}
# a declaration doesn't start with a statement keyword or inside an initializer list
NOT_DECL = re.compile(r"^\s*(?:return|case|else|if|for|while|switch|do|goto|break|continue|using|typedef|template|friend|\{|\}|,|\.|\(|[A-Za-z_]\w*\s*\()")


def strip(code):
    """Comments and the insides of string and character literals removed, in one pass (a "/*" in a
    string or a '"' in a character literal must not start anything). Newlines are kept."""
    out = []
    i, n = 0, len(code)
    while i < n:
        c = code[i]
        two = code[i:i + 2]
        if two == "//":
            while i < n and code[i] != "\n":
                i += 1
        elif two == "/*":
            end = code.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("\n" * code.count("\n", i, end))
            i = end
        elif c in "\"'":
            out.append(c + c)
            i += 1
            while i < n and code[i] != c and code[i] != "\n":
                i += 2 if code[i] == "\\" else 1
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def private_names(code):
    """Names declared at the top level of an anonymous namespace, or static at file scope."""
    names = set()
    depth = 0
    anon = []  # depths at which anonymous namespaces opened
    pending_anon = False
    parens = 0  # open ( ... ) spanning lines: a signature's later parameters are not declarations
    for line in code.split("\n"):
        stripped = line.strip()
        if re.match(r"namespace\s*\{?$", stripped) or re.match(r"namespace\s*\{", stripped):
            pending_anon = True
        top_of_anon = anon and depth == anon[-1] + 1
        file_static = depth == 0 and stripped.startswith("static ")
        if (top_of_anon or file_static) and parens == 0 and not NOT_DECL.match(line):
            m = DECL.match(line)
            if m:
                n = m.group("type") or m.group("name")
                if n and n not in IGNORE:
                    names.add(n)
        for ch in line:
            if ch == "(":
                parens += 1
            elif ch == ")":
                parens = max(0, parens - 1)
            elif ch == "{":
                if pending_anon:
                    anon.append(depth)
                    pending_anon = False
                depth += 1
            elif ch == "}":
                depth -= 1
                if anon and depth == anon[-1]:
                    anon.pop()
    return names


def main():
    problems = 0
    for module in sorted(os.listdir(ROOT)):
        files = {}
        for dirpath, _, filenames in os.walk(os.path.join(ROOT, module)):
            for f in filenames:
                if f.endswith(".cpp"):
                    with open(os.path.join(dirpath, f), encoding="utf-8", errors="replace") as h:
                        files[f] = strip(h.read())
        words = {f: set(WORD.findall(c)) for f, c in files.items()}
        for f, code in sorted(files.items()):
            for name in sorted(private_names(code)):
                others = sorted(o for o in files if o != f and name in words[o])
                if others:
                    problems += 1
                    print(f"{module}: '{name}' is private to {f} but also used in {', '.join(others)}")
    print(f"{problems} possible unity-build clashes")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
