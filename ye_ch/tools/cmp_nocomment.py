#!/usr/bin/env python3
"""
cmp_nocomment.py —— 比对 ye_ch 和 no_ch 两份工程：去掉注释、空行、行尾空白后逐行比较

用法：python cmp_nocomment.py <ye_ch 的 CommGateway 目录> <no_ch 的 CommGateway 目录>
例：  python cmp_nocomment.py E:/code/80-81/claude/05/ye_ch/Projects/CommGateway E:/code/80-81/claude/05/no_ch/Projects/CommGateway
只比 src/*.c 和 inc/*.h；有差异的文件打印前几处不同的代码行。
"""
import difflib
import re
import sys
from pathlib import Path


def strip(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)     # 块注释
    text = re.sub(r"//[^\n]*", "", text)                   # 行注释
    lines = [re.sub(r"\s+", " ", ln).strip() for ln in text.splitlines()]
    return [ln for ln in lines if ln]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return
    a, b = Path(sys.argv[1]), Path(sys.argv[2])
    same = True
    for sub in ("src", "inc"):
        for fa in sorted((a / sub).glob("*.[ch]")):
            fb = b / sub / fa.name
            if not fb.exists():
                print("缺文件：%s" % fb)
                same = False
                continue
            la = strip(fa.read_text(encoding="utf-8", errors="replace"))
            lb = strip(fb.read_text(encoding="utf-8", errors="replace"))
            if la != lb:
                same = False
                print("不一致：%s/%s" % (sub, fa.name))
                diff = [d for d in difflib.unified_diff(la, lb, "ye_ch", "no_ch", n=0, lineterm="")
                        if d[:1] in "+-" and d[:3] not in ("+++", "---")]
                for d in diff[:10]:
                    print("    " + d)
    print("两份代码一致" if same else "有差异，按上面的提示同步")


if __name__ == "__main__":
    main()
