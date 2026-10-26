#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_deck.py —— 用「文化风PPT模板.pptx」生成《多进程并发聊天程序》演示文稿

设计说明：
  · 封面 / 目录 / 章节页 / 六宫格 / 结束页 直接复用模板原页，只换文字，保持文化风版式；
  · 新增的技术页沿用模板配色（墨绿 #5A7266、描金 #DEC785 / #AF8F47、米色纸面）
    与中文字体（标题华文中宋、正文微软雅黑、代码 Consolas），确保风格统一；
  · 演示数据（服务端日志、客户端屏幕、ps 进程树）来自 chat/docs 下真实运行录制。

用法：python build_deck.py
输出：多进程并发聊天程序_演示.pptx
"""
import os
import re
from copy import deepcopy

from lxml import etree
from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.shapes import MSO_CONNECTOR, MSO_SHAPE
from pptx.enum.text import MSO_ANCHOR, MSO_AUTO_SIZE, PP_ALIGN
from pptx.oxml.ns import qn
from pptx.util import Inches, Pt

# ------------------------------------------------------------------ 路径
# 本脚本放在 <项目根>/tools/ 下，其余文件都相对项目根定位
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.dirname(TOOLS_DIR)
TEMPLATE = os.path.join(WORK, "文化风PPT模板.pptx")
CHAT_DOCS = os.path.join(WORK, "chat", "docs")
OUTPUT = os.path.join(WORK, "多进程并发聊天程序_演示.pptx")

# ------------------------------------------------------------------ 配色
GREEN = RGBColor(0x5A, 0x72, 0x66)
GREEN_D = RGBColor(0x3E, 0x4F, 0x47)
GOLD = RGBColor(0xDE, 0xC7, 0x85)
GOLD_D = RGBColor(0xAF, 0x8F, 0x47)
RED = RGBColor(0x66, 0x1D, 0x2E)
WHITE = RGBColor(0xFF, 0xFF, 0xFF)
INK = RGBColor(0x33, 0x3A, 0x36)
BODY = RGBColor(0x41, 0x4B, 0x45)
GRAY = RGBColor(0x7A, 0x82, 0x7D)
PANEL = RGBColor(0xF7, 0xF5, 0xEE)
PANEL_G = RGBColor(0xEE, 0xF2, 0xEF)
PANEL_Y = RGBColor(0xFB, 0xF6, 0xE8)
PANEL_R = RGBColor(0xF9, 0xEF, 0xEB)
RULE = RGBColor(0xE2, 0xDC, 0xC8)

TERM_BG = RGBColor(0x25, 0x2F, 0x2A)
TERM_TXT = RGBColor(0xE7, 0xE3, 0xD7)
TERM_DIM = RGBColor(0x93, 0xA7, 0x9A)
TERM_SYS = RGBColor(0xD9, 0xC8, 0x8E)
TERM_ERR = RGBColor(0xE3, 0x9A, 0x92)
CODE_CMT = RGBColor(0x87, 0x99, 0x8C)
CODE_KW = RGBColor(0x9B, 0xC2, 0xAC)
CODE_STR = RGBColor(0xD8, 0xC0, 0x8A)

HEAD_F = "华文中宋"
BODY_F = "微软雅黑"
MONO = "Consolas"

PAGE_W = 13.333
L, R = 0.95, 12.38          # 内容区左右边界

# ------------------------------------------------------------------ 基础排版工具


def _set_ea(run, font):
    """python-pptx 只写 a:latin，中文字体需要额外补 a:ea / a:cs。"""
    rPr = run._r.get_or_add_rPr()
    for tag in ("a:ea", "a:cs"):
        el = rPr.find(qn(tag))
        if el is None:
            el = etree.SubElement(rPr, qn(tag))
        el.set("typeface", font)


def textbox(slide, x, y, w, h, anchor=MSO_ANCHOR.TOP, wrap=True, margins=(0, 0, 0, 0)):
    box = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    tf = box.text_frame
    tf.word_wrap = wrap
    tf.auto_size = MSO_AUTO_SIZE.NONE
    tf.vertical_anchor = anchor
    ml, mt, mr, mb = margins
    tf.margin_left = Inches(ml)
    tf.margin_top = Inches(mt)
    tf.margin_right = Inches(mr)
    tf.margin_bottom = Inches(mb)
    return box


def para(tf, first=False, before=0, after=0, line=1.2, align=PP_ALIGN.LEFT):
    p = tf.paragraphs[0] if first else tf.add_paragraph()
    p.alignment = align
    p.space_before = Pt(before)
    p.space_after = Pt(after)
    p.line_spacing = line
    return p


def run(p, text, size=14, color=INK, font=BODY_F, bold=False, italic=False):
    r = p.add_run()
    r.text = text
    f = r.font
    f.size = Pt(size)
    f.bold = bold
    f.italic = italic
    f.name = font
    f.color.rgb = color
    _set_ea(r, font)
    return r


def bullet(p, char="·", color=GOLD_D, indent=0.24):
    pPr = p._p.get_or_add_pPr()
    pPr.set("marL", str(int(Inches(indent))))
    pPr.set("indent", str(int(-Inches(indent))))
    buClr = etree.SubElement(pPr, qn("a:buClr"))
    clr = etree.SubElement(buClr, qn("a:srgbClr"))
    clr.set("val", str(color))
    buFont = etree.SubElement(pPr, qn("a:buFont"))
    buFont.set("typeface", BODY_F)
    buChar = etree.SubElement(pPr, qn("a:buChar"))
    buChar.set("char", char)


def shape(slide, kind, x, y, w, h, fill=None, line=None, line_w=1.0, radius=None,
          dash=None, rot=None):
    s = slide.shapes.add_shape(kind, Inches(x), Inches(y), Inches(w), Inches(h))
    if fill is None:
        s.fill.background()
    else:
        s.fill.solid()
        s.fill.fore_color.rgb = fill
    if line is None:
        s.line.fill.background()
    else:
        s.line.color.rgb = line
        s.line.width = Pt(line_w)
        if dash is not None:
            s.line.dash_style = dash
    if radius is not None and kind == MSO_SHAPE.ROUNDED_RECTANGLE:
        s.adjustments[0] = radius
    if rot is not None:
        s.rotation = rot
    s.shadow.inherit = False
    tf = s.text_frame
    tf.word_wrap = True
    tf.margin_left = tf.margin_right = Inches(0.06)
    tf.margin_top = tf.margin_bottom = Inches(0.03)
    return s


def arrow(slide, x1, y1, x2, y2, color=GOLD_D, width=1.2, both=False, head=True):
    c = slide.shapes.add_connector(MSO_CONNECTOR.STRAIGHT, Inches(x1), Inches(y1),
                                   Inches(x2), Inches(y2))
    c.line.color.rgb = color
    c.line.width = Pt(width)
    ln = c.line._get_or_add_ln()
    if both:
        e = etree.SubElement(ln, qn("a:headEnd"))
        e.set("type", "triangle")
        e.set("w", "sm")
        e.set("len", "sm")
    if head:
        e = etree.SubElement(ln, qn("a:tailEnd"))
        e.set("type", "triangle")
        e.set("w", "sm")
        e.set("len", "sm")
    return c


def add_notes(slide, text):
    slide.notes_slide.notes_text_frame.text = text.strip()


# ------------------------------------------------------------------ 页面骨架

def page_header(slide, title, sub=None):
    shape(slide, MSO_SHAPE.DIAMOND, 0.98, 0.60, 0.20, 0.20, fill=GOLD, line=None)
    tb = textbox(slide, 1.34, 0.40, 10.6, 0.62, anchor=MSO_ANCHOR.MIDDLE)
    p = tb.text_frame.paragraphs[0]
    run(p, title, 26, GREEN, HEAD_F)
    if sub:
        run(p, "   " + sub, 12, GOLD_D, BODY_F)
    shape(slide, MSO_SHAPE.RECTANGLE, 0.98, 1.145, 0.95, 0.05, fill=GOLD_D, line=None)
    shape(slide, MSO_SHAPE.RECTANGLE, 1.93, 1.158, R - 1.93, 0.018, fill=GOLD, line=None)


def page_footer(slide, number):
    tb = textbox(slide, L, 6.98, 6.0, 0.30)
    run(tb.text_frame.paragraphs[0], "操作系统 · 小班展示", 9.5, GRAY, BODY_F)
    tb = textbox(slide, R - 0.8, 6.98, 0.8, 0.30)
    run(tb.text_frame.paragraphs[0], "%02d" % number, 10, GRAY, BODY_F)
    tb.text_frame.paragraphs[0].alignment = PP_ALIGN.RIGHT


def subtitle_note(slide, y, text, color=GOLD_D, size=11.5):
    tb = textbox(slide, L, y, R - L, 0.36)
    p = tb.text_frame.paragraphs[0]
    run(p, text, size, color, BODY_F)
    return tb


# ------------------------------------------------------------------ 表格（用形状画，避免主题边框干扰）

def grid_table(slide, x, y, col_w, rows, row_h, size=11, head_size=11.5,
               header_fill=GREEN, aligns=None, zebra=(WHITE, PANEL), head_h=0.34):
    heights = [head_h] + [row_h] * (len(rows) - 1)
    cy = y
    for r, row in enumerate(rows):
        h = heights[r]
        cx = x
        for c, val in enumerate(row):
            width = col_w[c]
            is_head = (r == 0)
            bg = header_fill if is_head else zebra[(r - 1) % 2]
            shape(slide, MSO_SHAPE.RECTANGLE, cx, cy, width, h, fill=bg, line=None)
            tb = textbox(slide, cx + 0.10, cy, width - 0.16, h, anchor=MSO_ANCHOR.MIDDLE)
            p = tb.text_frame.paragraphs[0]
            p.alignment = aligns[c] if aligns else PP_ALIGN.LEFT
            run(p, val, head_size if is_head else size,
                WHITE if is_head else INK, BODY_F, bold=is_head)
            cx += width
        if r > 0:
            shape(slide, MSO_SHAPE.RECTANGLE, x, cy + h - 0.008, sum(col_w), 0.008,
                  fill=RULE, line=None)
        cy += h


# ------------------------------------------------------------------ 代码 / 终端面板

CODE_KWS = ("if", "else", "for", "while", "return", "int", "char", "void",
            "pid_t", "break", "continue", "switch", "case", "static", "const")


def terminal_lines(slide, x, y, w, h, lines, size=10, bg=TERM_BG, pad=0.14):
    shape(slide, MSO_SHAPE.ROUNDED_RECTANGLE, x, y, w, h, fill=bg, line=None, radius=0.02)
    tb = textbox(slide, x + pad, y + pad, w - 2 * pad, h - 2 * pad)
    tf = tb.text_frame
    for i, line in enumerate(lines):
        p = para(tf, first=(i == 0), line=1.12)
        color = TERM_TXT
        if line.startswith("***"):
            color = TERM_SYS
        elif line.startswith("!!!"):
            color = TERM_ERR
        elif line.startswith("已连接") or line.startswith("----"):
            color = TERM_DIM
        m = re.match(r"^(\[服务端 \d\d:\d\d:\d\d\])(.*)$", line)
        if m:
            run(p, m.group(1), size, TERM_DIM, MONO)
            run(p, m.group(2), size, TERM_TXT, MONO)
        else:
            run(p, line, size, color, MONO)


def code_block(slide, x, y, w, h, code, size=10, title=None):
    shape(slide, MSO_SHAPE.ROUNDED_RECTANGLE, x, y, w, h, fill=TERM_BG, line=None, radius=0.02)
    top = y + 0.10
    if title:
        tb = textbox(slide, x + 0.16, top, w - 0.3, 0.26)
        run(tb.text_frame.paragraphs[0], title, 9.5, TERM_DIM, MONO)
        top += 0.30
    tb = textbox(slide, x + 0.16, top, w - 0.3, h - (top - y) - 0.10)
    tf = tb.text_frame
    for i, line in enumerate(code.split("\n")):
        p = para(tf, first=(i == 0), line=1.06)
        cut = line.find("/*")
        if cut < 0:
            cut = line.find("//")
        if cut >= 0:
            head, tail = line[:cut], line[cut:]
            if head:
                _code_runs(p, head, size)
            run(p, tail, size, CODE_CMT, MONO, italic=True)
        else:
            _code_runs(p, line, size)


def _code_runs(p, text, size):
    parts = re.split(r"(\b(?:" + "|".join(CODE_KWS) + r")\b)", text)
    for tok in parts:
        if not tok:
            continue
        if tok in CODE_KWS:
            run(p, tok, size, CODE_KW, MONO)
        elif re.fullmatch(r'"[^"]*"', tok):
            run(p, tok, size, CODE_STR, MONO)
        else:
            run(p, tok, size, TERM_TXT, MONO)


# ------------------------------------------------------------------ 幻灯片复制 / 删除 / 排序

def delete_slide(prs, slide):
    lst = prs.slides._sldIdLst
    for el in list(lst):
        rId = el.get(qn("r:id"))
        if prs.part.related_part(rId) is slide.part:
            prs.part.drop_rel(rId)
            lst.remove(el)
            return


def duplicate_slide(prs, src):
    new = prs.slides.add_slide(src.slide_layout)
    for sh in list(new.shapes):
        sh._element.getparent().remove(sh._element)

    id_map = {}
    for rId, rel in src.part.rels.items():
        if rel.reltype.endswith("/slideLayout") or rel.reltype.endswith("/notesSlide"):
            continue
        if rel.is_external:
            id_map[rId] = new.part.rels.get_or_add_ext_rel(rel.reltype, rel.target_ref)
        else:
            id_map[rId] = new.part.rels.get_or_add(rel.reltype, rel.target_part)

    for sh in src.shapes:
        el = deepcopy(sh._element)
        for node in el.iter():
            for attr in (qn("r:embed"), qn("r:id"), qn("r:link")):
                old = node.get(attr)
                if old in id_map:
                    node.set(attr, id_map[old])
        new.shapes._spTree.append(el)
    return new


def reorder(prs, ordered):
    lst = prs.slides._sldIdLst
    el_by_part = {}
    for el in list(lst):
        rId = el.get(qn("r:id"))
        el_by_part[prs.part.related_part(rId).partname] = el
    for el in list(lst):
        lst.remove(el)
    for s in ordered:
        lst.append(el_by_part[s.part.partname])


def set_text(shape_, text):
    """替换整个文本框的文字，保留第一段第一个 run 的格式。"""
    tf = shape_.text_frame
    first = tf.paragraphs[0]
    if not first.runs:
        first.add_run().text = text
    else:
        first.runs[0].text = text
        for extra in first.runs[1:]:
            extra._r.getparent().remove(extra._r)
    for extra in list(tf.paragraphs)[1:]:
        extra._p.getparent().remove(extra._p)


def find_shape(slide, name):
    for sh in slide.shapes:
        if sh.name == name:
            return sh
    raise KeyError(name)


# ------------------------------------------------------------------ 读取真实演示数据

def read_transcript(path):
    blocks, cur = {}, None
    for line in open(path, encoding="utf-8").read().splitlines():
        m = re.match(r"^【(.+?)】$", line.strip())
        if m:
            cur = m.group(1)
            blocks[cur] = []
        elif cur is not None:
            if set(line.strip()) <= set("-"):
                continue
            blocks[cur].append(line)
    return {k: "\n".join(v).strip("\n") for k, v in blocks.items()}


def parse_ps(path):
    rows = []
    for line in open(path, encoding="utf-8"):
        parts = line.split()
        if len(parts) < 5 or not parts[1].isdigit() or not parts[2].isdigit():
            continue
        rows.append((int(parts[1]), int(parts[2]), parts[-1]))
    return rows


SRV_LOG = read_transcript(os.path.join(CHAT_DOCS, "demo_transcript.txt"))["服务端日志"]
CLIENT1 = read_transcript(os.path.join(CHAT_DOCS, "demo_transcript.txt"))["客户端1(小明) 的屏幕"]
CLIENT2 = read_transcript(os.path.join(CHAT_DOCS, "demo_transcript.txt"))["客户端2(alice) 的屏幕"]
PS_ROWS = parse_ps(os.path.join(CHAT_DOCS, "ps_tree.txt"))

# 主进程 = 不是任何进程的子进程的那个 server
_srv = [(pid, ppid) for pid, ppid, cmd in PS_ROWS if "server" in cmd]
_server_pids = {pid for pid, _ in _srv}
_ppids = [ppid for _pid, ppid in _srv if ppid in _server_pids]
MAIN_PID = _ppids[0] if _ppids else sorted(_server_pids)[0]
# pid -> 昵称（按服务端日志里的登录顺序）
_pid_nick = dict(re.findall(r"子进程 pid=(\d+) 完成登录，昵称=(\S+?)，", SRV_LOG))
# 槽位 -> pid：按接入顺序（槽位 0、1、2）排列，图中客户端 A/B/C 与之一一对应
_slot_pid = {int(slot): int(pid) for pid, slot
             in re.findall(r"创建子进程 pid=(\d+)，占用槽位 (\d+)", SRV_LOG)}
CHILD_PIDS = [_slot_pid[slot] for slot in sorted(_slot_pid)]
CHILD_NICKS = [_pid_nick.get(str(pid), "客户端") for pid in CHILD_PIDS]
MAIN_LOG_LINE = SRV_LOG.splitlines()[0]

# ------------------------------------------------------------------ 开始构建
prs = Presentation(TEMPLATE)
all_slides = list(prs.slides)
cover, toc, sec_tpl = all_slides[0], all_slides[1], all_slides[2]
six, thanks = all_slides[4], all_slides[9]

# 先记住要删掉的模板示例页（等新页全部建好后再删，避免新页 partname 与旧页撞名）
trash = [all_slides[i] for i in (3, 5, 6, 7, 8)]

NEW = prs.slide_layouts[6]      # 空白版式


def new_slide():
    s = prs.slides.add_slide(NEW)
    for sh in list(s.shapes):
        sh._element.getparent().remove(sh._element)
    return s


# ============================================================ 01 封面
set_text(find_shape(cover, "文本框 7"), "多进程并发聊天程序")
set_text(find_shape(cover, "文本框 26"), "操作系统 · 小班展示")
set_text(find_shape(cover, "文本框 47"), "2026 年 10 月")
for sh in cover.shapes:
    if sh.name == "文本框 7":
        for p in sh.text_frame.paragraphs:
            for r in p.runs:
                r.font.size = Pt(60)
    if sh.name == "文本框 26":
        sh.width = Inches(2.20)
        for p in sh.text_frame.paragraphs:
            for r in p.runs:
                r.font.size = Pt(15)
    if sh.name == "文本框 47":
        sh.width = Inches(2.30)
        for p in sh.text_frame.paragraphs:
            for r in p.runs:
                r.font.size = Pt(15)
tb = textbox(cover, 4.62, 2.66, 8.2, 0.46)
run(tb.text_frame.paragraphs[0],
    "socket + fork：一个服务端，多个客户端，一个客户端一个进程", 18, GOLD_D, BODY_F)
add_notes(cover, """
开场：这次小班展示我做的是一个"多进程并发聊天程序"。
只用 TCP socket 和最基础的进程原语 fork/select/signal，不依赖任何第三方库。
演示目标：一个服务端可以被多个客户端同时连上，每来一个客户端，服务端就 fork 一个子进程专门服务它。
""")

# ============================================================ 02 目录
toc_items = ["需求与架构", "进程模型", "协议与代码", "演示与可视化"]
for name, text in zip(["文本框 38", "文本框 43", "文本框 47", "文本框 51"], toc_items):
    set_text(find_shape(toc, name), text)
# 模板里 "CONTENT" 用的是缺失的 GEETYPE 装饰字体（渲染成怪字形），
# 且 32pt 下会折成两行，这里换成通用字体并缩小到 20pt
_content = find_shape(toc, "文本框 16")
_content.width = Inches(2.60)
for p in _content.text_frame.paragraphs:
    for r in p.runs:
        r.font.size = Pt(20)
        r.font.name = BODY_F
        r.font.color.rgb = RGBColor(0xD5, 0xCF, 0xBE)
        _set_ea(r, BODY_F)
add_notes(toc, """
汇报分四部分：
1. 先讲清楚需求是什么、整体架构长什么样；
2. 再讲最核心的进程模型：为什么一客户端一进程、父子进程怎么配合；
3. 然后是通信协议和关键代码（含一套协议两种渲染方式）；
4. 最后跑真实程序，看命令行客户端、Windows 图形客户端和服务端实时看板，
   再用自动化测试收尾。
""")

# ============================================================ 章节页工厂
def make_section(number, title, notes, page_no):
    s = duplicate_slide(prs, sec_tpl)
    # 复制出来的页面会带着模板页里已有的页脚（"操作系统 · 小班展示" 和页码），先清掉
    for sh in list(s.shapes):
        if not sh.has_text_frame:
            continue
        t = sh.text_frame.text.strip()
        if sh.name.startswith("TextBox") and (t == "操作系统 · 小班展示" or re.fullmatch(r"\d{2}", t)):
            sh._element.getparent().remove(sh._element)
    set_text(find_shape(s, "文本框 22"), number)
    set_text(find_shape(s, "文本框 20"), title)
    for sh in s.shapes:
        if sh.name == "文本框 20":
            for p in sh.text_frame.paragraphs:
                for r in p.runs:
                    r.font.size = Pt(30)
    page_footer(s, page_no)
    add_notes(s, notes)
    return s


# ============================================================ 03 章节页 01（复用模板原页）
set_text(find_shape(sec_tpl, "文本框 22"), "01")
set_text(find_shape(sec_tpl, "文本框 20"), "需求与总体架构")
for sh in sec_tpl.shapes:
    if sh.name == "文本框 20":
        for p in sh.text_frame.paragraphs:
            for r in p.runs:
                r.font.size = Pt(30)
page_footer(sec_tpl, 3)
add_notes(sec_tpl, """
第一部分：需求与总体架构。
需求其实就一句话：一个服务端能同时服务多个客户端，每来一个客户端，服务端 fork 一个子进程。
接下来先看功能，再看架构怎么落地这个需求。
""")


# ============================================================ 04 六个核心功能（复用六宫格）
set_text(find_shape(six, "文本框 38"), "并发接入")
set_text(find_shape(six, "文本框 53"), "群聊广播")
set_text(find_shape(six, "文本框 63"), "定向私聊")
set_text(find_shape(six, "文本框 52"), "在线名单")
set_text(find_shape(six, "文本框 54"), "昵称管理")
set_text(find_shape(six, "文本框 73"), "系统消息")

six_titles = ["文本框 38", "文本框 53", "文本框 63", "文本框 52", "文本框 54", "文本框 73"]
for name in six_titles:
    sh = find_shape(six, name)
    tf = sh.text_frame
    tf.word_wrap = False
    for p in tf.paragraphs:
        p.alignment = PP_ALIGN.CENTER
        for r in p.runs:
            r.font.size = Pt(16)
    sh.text_frame.vertical_anchor = MSO_ANCHOR.MIDDLE

six_bodies = {
    "文本框 4":  "accept 之后立刻 fork 子进程，最多 64 路并发互不阻塞",
    "文本框 3":  "消息经主进程转发，所有在线用户都能收到",
    "文本框 2":  "只有指定昵称的客户端能收到内容",
    "文本框 114": "/list 实时查看在线用户与人数",
    "文本框 117": "登录查重，/nick 随时改名",
    "文本框 118": "上线、离开、改名都会自动广播",
}
for name, text in six_bodies.items():
    sh = find_shape(six, name)
    tf = sh.text_frame
    tf.word_wrap = True
    p = tf.paragraphs[0]
    p.alignment = PP_ALIGN.LEFT
    if p.runs:
        p.runs[0].text = text
        for extra in p.runs[1:]:
            extra._r.getparent().remove(extra._r)
    for extra in list(tf.paragraphs)[1:]:
        extra._p.getparent().remove(extra._p)
    for r in p.runs:
        r.font.size = Pt(13.5)
        r.font.color.rgb = BODY
        _set_ea(r, BODY_F)

page_header(six, "六个核心功能")
page_footer(six, 4)
add_notes(six, """
先看功能面：这些功能都是围绕"多人同时在线聊天"展开的。
左边三个是服务端能力：并发接入、群聊广播、定向私聊；
右边三个是用户体验：在线名单、昵称管理、上下线通知。
下面所有功能都由同一套进程模型支撑，不是每个功能单独实现的。
""")

# ============================================================ 05 总体架构（新页）
s = new_slide()
page_header(s, "总体架构：一个客户端一个进程")
page_footer(s, 5)

tb = textbox(s, L, 1.40, 2.0, 0.3)
run(tb.text_frame.paragraphs[0], "客户端", 11, GOLD_D, BODY_F, bold=True)
tb = textbox(s, 3.40, 1.40, 2.2, 0.3)
run(tb.text_frame.paragraphs[0], "子进程（fork 出来）", 11, GOLD_D, BODY_F, bold=True)
tb = textbox(s, 6.28, 1.40, 2.9, 0.3)
run(tb.text_frame.paragraphs[0], "主进程（唯一的调度者）", 11, GOLD_D, BODY_F, bold=True)

row_y = [1.78, 3.00, 4.22]
for i, y in enumerate(row_y):
    c = shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, L, y, 1.78, 0.92,
              fill=PANEL, line=GOLD_D, line_w=0.9, radius=0.10)
    tf = c.text_frame
    tf.vertical_anchor = MSO_ANCHOR.MIDDLE
    p = tf.paragraphs[0]
    p.alignment = PP_ALIGN.CENTER
    run(p, "客户端 %s" % "ABC"[i], 13, GREEN_D, BODY_F, bold=True)
    p2 = tf.add_paragraph()
    p2.alignment = PP_ALIGN.CENTER
    run(p2, CHILD_NICKS[i] if i < len(CHILD_NICKS) else "-", 10, GRAY, BODY_F)

    arrow(s, L + 1.82, y + 0.46, 3.30, y + 0.46, GOLD_D, 1.2)

    ch = shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, 3.34, y, 2.10, 0.92,
               fill=PANEL_G, line=GREEN, line_w=1.1, radius=0.10)
    tf = ch.text_frame
    tf.vertical_anchor = MSO_ANCHOR.MIDDLE
    p = tf.paragraphs[0]
    p.alignment = PP_ALIGN.CENTER
    run(p, "子进程", 12.5, GREEN, BODY_F, bold=True)
    p2 = tf.add_paragraph()
    p2.alignment = PP_ALIGN.CENTER
    run(p2, "pid = %s" % (CHILD_PIDS[i] if i < len(CHILD_PIDS) else "-"), 9.5, GRAY, MONO)

    # 子进程 -> 主进程（双向）
    line = s.shapes.add_connector(MSO_CONNECTOR.STRAIGHT, Inches(5.56), Inches(y + 0.46),
                                  Inches(6.30), Inches(y + 0.46))
    line.line.color.rgb = GOLD
    line.line.width = Pt(1.6)

tb = textbox(s, 2.76, 1.88, 0.52, 0.24)
run(tb.text_frame.paragraphs[0], "TCP", 8.5, GOLD_D, MONO)
tb = textbox(s, 5.60, 5.30, 2.4, 0.30)
run(tb.text_frame.paragraphs[0], "socketpair 全双工", 9.5, GOLD_D, BODY_F)

main_box = shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, 6.30, 1.78, 2.95, 3.36,
                 fill=WHITE, line=GREEN, line_w=1.6, radius=0.06)
tf = main_box.text_frame
tf.vertical_anchor = MSO_ANCHOR.TOP
tf.margin_top = Inches(0.16)
p = tf.paragraphs[0]
p.alignment = PP_ALIGN.CENTER
run(p, "主进程", 15, GREEN, HEAD_F, bold=True)
p2 = tf.add_paragraph()
p2.alignment = PP_ALIGN.CENTER
run(p2, "pid = %d" % MAIN_PID, 9.5, GRAY, MONO)
for text in ["accept() 接入新连接", "维护用户表 / 昵称查重",
             "select() 转发与广播", "SIGCHLD 回收子进程"]:
    p3 = tf.add_paragraph()
    p3.alignment = PP_ALIGN.LEFT
    p3.space_before = Pt(6)
    p3.line_spacing = 1.15
    bullet(p3, "·", GOLD_D, 0.18)
    run(p3, text, 11, BODY, BODY_F)

# 右侧说明
note = shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, 9.55, 1.78, 2.83, 3.36, fill=PANEL, line=None, radius=0.06)
tf = note.text_frame
tf.margin_left = tf.margin_right = Inches(0.16)
tf.margin_top = Inches(0.14)
p = tf.paragraphs[0]
run(p, "为什么这样划分", 12, GREEN, BODY_F, bold=True)
for text in ["主进程不持有客户端连接，只做调度",
             "会话互相隔离，一个卡住不影响别人",
             "全局状态集中在主进程，不用加锁",
             "代价：跨进程要靠 IPC 传消息"]:
    p2 = tf.add_paragraph()
    p2.space_before = Pt(7)
    p2.line_spacing = 1.18
    bullet(p2, "·", GOLD_D, 0.18)
    run(p2, text, 10.5, BODY, BODY_F)

# 底部要点
bar = shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, L, 5.72, R - L, 0.82, fill=PANEL_G, line=None, radius=0.08)
tf = bar.text_frame
tf.margin_left = Inches(0.18)
tf.vertical_anchor = MSO_ANCHOR.MIDDLE
p = tf.paragraphs[0]
run(p, "进程之间不共享地址空间", 12, GREEN, BODY_F, bold=True)
run(p, "——所以「谁在线」这张用户表只能由主进程维护，子进程通过 socketpair 请求它代为转发。",
    12, BODY, BODY_F)

add_notes(s, """
这张图是整个程序的结构：左边是客户端，中间是 fork 出来的子进程，右边是主进程。
注意三点：
1）客户端只连到"自己那个子进程"，连接是 fork 前后由子进程继承并持有的；
2）主进程不持有任何客户端连接，它只持有和各个子进程之间的控制通道；
3）父子进程之间的通道是 socketpair，本质是 AF_UNIX 的全双工套接字，可以双向传帧。
图上的 pid 都是真实运行抓下来的，数字来自 docs/ps_tree.txt。
""")

# ============================================================ 06 章节 02
sec2 = make_section("02", "进程模型与并发", """
第二部分讲进程模型，这也是这次演示的核心：为什么每个客户端要一个进程，
fork 之后父子进程各自负责什么，以及进程退出了怎么回收。
""", 6)

# ============================================================ 07 三个角色（新页）
s = new_slide()
page_header(s, "三个角色：主进程 · 子进程 · 控制通道")
page_footer(s, 7)

cards = [
    ("01", "主进程：接入与调度", [
        "只做 accept() 与消息转发，不直接读写客户端数据",
        "维护在线用户表、昵称查重、广播路由",
        "用 select() 同时盯住所有控制通道",
        "收到 SIGCHLD 后 waitpid 回收子进程",
        "服务器启停、最大连接数控制",
    ]),
    ("02", "子进程：独占一条连接", [
        "每接入一个客户端就 fork 一个子进程",
        "负责该连接的收发与终端渲染",
        "客户端断开即自行退出，由父进程回收",
        "单个会话出错不影响其他会话",
        "退出前把 QUIT 请求交给主进程",
    ]),
    ("03", "控制通道：socketpair", [
        "AF_UNIX 全双工，父子进程各持一端",
        "子进程上交请求，主进程下发消息",
        "每个子进程一条专属通道，天然串行",
        "进程间不共享内存，无需考虑加锁",
        "帧头 3 字节，接收方按长度分帧",
    ]),
]
card_x = [L, 4.87, 8.79]
for (num, title, items), x in zip(cards, card_x):
    shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, x, 1.55, 3.63, 4.35,
          fill=WHITE, line=GREEN, line_w=1.0, radius=0.05)
    badge = shape(s, MSO_SHAPE.OVAL, x + 0.26, 1.78, 0.62, 0.62, fill=GREEN, line=None)
    tf = badge.text_frame
    tf.vertical_anchor = MSO_ANCHOR.MIDDLE
    p = tf.paragraphs[0]
    p.alignment = PP_ALIGN.CENTER
    run(p, num, 12, GOLD, MONO, bold=True)

    tb = textbox(s, x + 0.98, 1.86, 2.5, 0.5, anchor=MSO_ANCHOR.MIDDLE)
    run(tb.text_frame.paragraphs[0], title, 14, GREEN, BODY_F, bold=True)

    tb = textbox(s, x + 0.28, 2.66, 3.10, 3.05)
    tf = tb.text_frame
    for i, text in enumerate(items):
        p = para(tf, first=(i == 0), before=0 if i == 0 else 10, line=1.24)
        bullet(p, "▪", GOLD_D, 0.20)
        run(p, text, 12, BODY, BODY_F)

bar = shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, L, 6.12, R - L, 0.62, fill=PANEL_Y, line=None, radius=0.10)
tf = bar.text_frame
tf.margin_left = Inches(0.18)
tf.vertical_anchor = MSO_ANCHOR.MIDDLE
run(tf.paragraphs[0], "一句话总结：子进程管「连接」，主进程管「关系」。", 12, GREEN, BODY_F, bold=True)
add_notes(s, """
把职责拆成三个角色，程序就好讲了：
主进程是"接线员+调度员"，它不碰聊天内容，只决定"这条消息该发给谁"；
子进程是"专职客服"，一个客户端一个，读写、渲染、退出都在自己进程里完成；
中间那条 socketpair 是"内部热线"，子进程不能直接改用户表，只能把请求交给主进程。
""")

# ============================================================ 08 生命周期（新页）
s = new_slide()
page_header(s, "一次连接的完整生命周期")
page_footer(s, 8)

steps = [("accept()", "主进程", PANEL_G, GREEN),
         ("socketpair()", "主进程", PANEL_G, GREEN),
         ("fork()", "主进程", PANEL_G, GREEN),
         ("子进程服务", "子进程", PANEL_Y, GOLD_D),
         ("客户端断开", "子进程", PANEL_Y, GOLD_D),
         ("waitpid 回收", "SIGCHLD", PANEL_R, RED)]
step_w, step_h, gap = 1.68, 0.92, 0.22
x = L
for i, (name, who, bg, color) in enumerate(steps):
    box = shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, x, 1.62, step_w, step_h,
                fill=bg, line=color, line_w=1.0, radius=0.10)
    tf = box.text_frame
    tf.vertical_anchor = MSO_ANCHOR.MIDDLE
    p = tf.paragraphs[0]
    p.alignment = PP_ALIGN.CENTER
    run(p, name, 11.5, INK, MONO if "(" in name else BODY_F, bold=True)
    p2 = tf.add_paragraph()
    p2.alignment = PP_ALIGN.CENTER
    run(p2, who, 9, GRAY, BODY_F)
    if i < len(steps) - 1:
        arrow(s, x + step_w + 0.02, 1.62 + step_h / 2, x + step_w + gap - 0.02,
              1.62 + step_h / 2, GOLD_D, 1.1)
    x += step_w + gap

tb = textbox(s, L, 2.72, R - L, 0.34)
run(tb.text_frame.paragraphs[0],
    "fork() 的返回值是区分父子进程的唯一依据：< 0 失败、= 0 在子进程、> 0 在父进程（拿到子进程 pid）",
    11.5, GOLD_D, BODY_F)

notes = [
    ("①", "fork 之前先 fflush(NULL)：否则父进程 stdin/stdout 缓冲区会被复制一份，父子各打印一遍"),
    ("②", "子进程立刻 close 掉 listen fd 和其它会话的控制通道：描述符泄漏会让端口释放不掉"),
    ("③", "子进程用 _exit() 退出：不执行父进程注册的 atexit，也不重复刷新缓冲区"),
    ("④", "SIGCHLD 处理函数里循环 waitpid(-1, NULL, WNOHANG)：不回收就会积累僵尸进程"),
]
y = 3.30
for num, text in notes:
    tb = textbox(s, L, y, 0.42, 0.34)
    run(tb.text_frame.paragraphs[0], num, 14, GOLD_D, BODY_F, bold=True)
    tb = textbox(s, L + 0.44, y + 0.02, R - L - 0.5, 0.62)
    run(tb.text_frame.paragraphs[0], text, 12, BODY, BODY_F)
    shape(s, MSO_SHAPE.RECTANGLE, L, y + 0.62, R - L, 0.010, fill=RULE, line=None)
    y += 0.80

add_notes(s, """
这一页是"生命周期"，也是老师最可能追问的地方：
1) accept 之后 socketpair 建立控制通道，然后 fork；
2) 子进程只保留自己那一端，开始服务；
3) 客户端断开，子进程读到 EOF，通知主进程后退出；
4) 主进程收到 SIGCHLD，用 waitpid(WNOHANG) 循环回收，避免僵尸。
下面四条是 fork 之后必须做对的四件事，任何一条漏掉都会出奇怪的问题。
""")

# ============================================================ 09 章节 03
sec3 = make_section("03", "通信协议与代码", """
第三部分看协议和代码。先看我们自己定义的帧格式，再看两个最关键的代码片段：
accept+fork，以及用 select 做集中路由。
""", 9)

# ============================================================ 10 协议（新页）
s = new_slide()
page_header(s, "通信协议：帧格式与消息类型")
page_footer(s, 10)

fbox_y, fbox_h = 1.52, 0.78
fields = [("type", "1 字节", L, 1.55, PANEL_G, GREEN),
          ("len", "2 字节（大端）", 2.50, 1.85, PANEL_Y, GOLD_D),
          ("payload", "0 ~ 65535 字节", 4.35, 3.05, PANEL, GREEN_D)]
for name, desc, x, w, bg, color in fields:
    shape(s, MSO_SHAPE.RECTANGLE, x, fbox_y, w, fbox_h, fill=bg, line=color, line_w=1.0, dash=None)
    tb = textbox(s, x, fbox_y + 0.06, w, 0.34)
    p = tb.text_frame.paragraphs[0]
    p.alignment = PP_ALIGN.CENTER
    run(p, name, 12, GREEN_D, MONO, bold=True)
    tb = textbox(s, x, fbox_y + 0.40, w, 0.30)
    p = tb.text_frame.paragraphs[0]
    p.alignment = PP_ALIGN.CENTER
    run(p, desc, 9.5, GRAY, BODY_F)

tb = textbox(s, L, 2.36, 6.5, 0.30)
run(tb.text_frame.paragraphs[0], "帧头 3 字节 + 负载：接收方先读帧头，再按长度读满负载",
    10.5, GOLD_D, BODY_F)

tb = textbox(s, 8.05, 1.50, R - 8.05, 1.10)
tf = tb.text_frame
p = tf.paragraphs[0]
run(p, "为什么要自己切帧？", 12, GREEN, BODY_F, bold=True)
p = tf.add_paragraph()
p.space_before = Pt(4)
p.line_spacing = 1.2
run(p, "TCP 只保证字节流有序：一次 read 可能只拿到半帧，也可能两帧粘在一起。帧头里的长度字段让接收方能够切分消息。",
    10.5, BODY, BODY_F)

rows = [["帧类型", "方向", "用途"],
        ["LOGIN", "C → S", "登记昵称；已登录时表示改名"],
        ["TALK", "C → S", "群聊正文，广播给所有在线用户"],
        ["PRIVATE", "C → S", "私聊，负载为「昵称 \\t 正文」"],
        ["LIST", "C → S", "请求在线用户列表"],
        ["QUIT", "C → S", "主动退出，服务端回 BYE"],
        ["SAY", "S → C", "群聊消息，负载为「时间 \\t 昵称 \\t 正文」"],
        ["WHISPER", "S → C", "私聊抵达，只有目标客户端收到"],
        ["SYS", "S → C", "系统提示：上线、离开、改名、服务器关闭"],
        ["ERR", "S → C", "错误提示：昵称非法、用户不在线等"],
        ["USERS", "S → C", "在线列表，昵称以 \\n 分隔，子进程渲染成列表"],
        ["TEXT", "子 → 客", "已渲染好的一行彩色文本，客户端直接打印"],
        ["BYE", "S → C", "连接结束，客户端收到后退出"]]
grid_table(s, L, 2.78, [1.55, 0.95, 8.93], rows, 0.285, size=10.5, head_size=11)

add_notes(s, """
协议只用一条规则：三字节帧头 + 负载。帧头是类型和长度。
类型一共 12 个：前 5 个是客户端请求，后 7 个是服务端应答。
这样设计的好处是：客户端程序非常简单，收到 TEXT 直接打印就行，渲染逻辑全在服务端；
同时因为长度明确，粘包、半包都不会解析错。
""")

# ============================================================ 11 关键代码①
s = new_slide()
page_header(s, "关键代码 ①：accept 之后 fork 一个子进程")
page_footer(s, 11)

code1 = """\
/* 每接入一个客户端，就 fork 一个专属子进程 */
int sv[2];
socketpair(AF_UNIX, SOCK_STREAM, 0, sv);  /* 父子通道 */

fflush(NULL);                 /* 先清空 stdio 缓冲 */
pid = fork();
if (pid == 0) {               /* =0：这里是子进程 */
    close(sv[0]);             /* 只留自己那一端 */
    child_session(cfd, sv[1], peer);
    _exit(0);                 /* 不走父进程 atexit */
}
/* >0：父进程，pid 是子进程号 */
close(cfd);                   /* 连接交给子进程 */
close(sv[1]);
g_clients[idx].fd  = sv[0];
g_clients[idx].pid = pid;"""
code_block(s, L, 1.52, 6.55, 4.86, code1, size=11, title="server.c · do_accept()")

tb = textbox(s, 7.78, 1.52, R - 7.78, 4.9)
tf = tb.text_frame
points = [
    ("fd 是被「继承」的", "fork 之后父子进程各有一份描述符表，两个进程都以为自己在用同一个连接——这正是必须精心 close 的原因。"),
    ("父进程要松手", "父进程 close(cfd) 只是把引用计数减一，客户端连接仍然由子进程持有，不会断。"),
    ("子进程要清场", "监听套接字和兄弟会话的控制通道都是继承来的，必须关掉，否则端口和连接都释放不掉。"),
    ("进程退出方式", "_exit() 不刷新缓冲区、不执行 atexit，避免父进程未输出的内容被子进程重打一遍。"),
]
for i, (head, body) in enumerate(points):
    p = para(tf, first=(i == 0), before=0 if i == 0 else 13, line=1.22)
    bullet(p, "▪", GOLD_D, 0.20)
    run(p, head + "：", 12, GREEN, BODY_F, bold=True)
    run(p, body, 11.5, BODY, BODY_F)

add_notes(s, """
这是整个演示最核心的十几行。
socketpair 先建通道，fflush，再 fork。
fork 返回 0 就在子进程里：关掉父端，进入子进程主循环，退出用 _exit；
返回大于 0 就在父进程里：关掉客户端连接和自己的对端，把子进程 pid 记进用户表。
注意"两个进程都以为自己持有同一个连接"，这是理解多进程服务器的关键。
""")

# ============================================================ 12 关键代码②
s = new_slide()
page_header(s, "关键代码 ②：一个 select 盯住所有通道")
page_footer(s, 12)

code2 = """\
/* 主进程：同时等待监听套接字和 N 条控制通道 */
FD_ZERO(&rfds);
FD_SET(g_listen_fd, &rfds);          /* 新连接 */
for (i = 0; i < CHAT_MAX_CLIENTS; i++)
    if (g_clients[i].fd >= 0)
        FD_SET(g_clients[i].fd, &rfds);   /* 会话 */

select(maxfd + 1, &rfds, NULL, NULL, NULL);

if (FD_ISSET(g_listen_fd, &rfds))
    do_accept();                     /* 新连接 -> fork */

for (i = 0; i < CHAT_MAX_CLIENTS; i++) {
    if (!FD_ISSET(g_clients[i].fd, &rfds))
        continue;
    frame_recv(g_clients[i].fd, &type, payload,
               sizeof payload, &len); /* 读一帧 */
    handle_request(i, type, payload, len);
}"""
code_block(s, L, 1.52, 6.55, 4.86, code2, size=11, title="server.c · main() 事件循环")

routes = [("群聊", "遍历用户表，逐个下发 SAY 帧"),
          ("私聊", "查到目标槽位，只给它发 WHISPER 帧"),
          ("在线列表", "当场拼装昵称列表，回 USERS 帧"),
          ("退出/掉线", "close 控制通道，广播离开消息")]
tb = textbox(s, 7.78, 1.50, R - 7.78, 0.34)
run(tb.text_frame.paragraphs[0], "消息如何路由", 13, GREEN, BODY_F, bold=True)
y = 1.92
for head, body in routes:
    shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, 7.78, y, R - 7.78, 0.72, fill=PANEL, line=None, radius=0.12)
    tb = textbox(s, 7.94, y + 0.10, R - 7.98, 0.56)
    p = tb.text_frame.paragraphs[0]
    run(p, head, 11.5, GREEN_D, BODY_F, bold=True)
    p2 = tb.text_frame.add_paragraph()
    run(p2, body, 9.5, BODY, BODY_F)
    y += 0.86

tb = textbox(s, 7.78, 5.62, R - 7.78, 0.70)
tf = tb.text_frame
p = tf.paragraphs[0]
p.line_spacing = 1.2
run(p, "主进程从不阻塞在某个客户端上：select 告诉我们谁有数据，处理完立刻回到 select。",
    10.5, GOLD_D, BODY_F)

add_notes(s, """
父进程的这一段是"调度中枢"：一个 select 同时等 N+1 个描述符。
有新连接就去 accept+fork；已有会话可读，就读一帧、交给 handle_request 决定发去哪里。
注意父进程是"非阻塞式"的设计思路：它不针对某一个客户端卡住，
所以即使某个客户端一直不发数据，也不影响其他人。
""")

# ============================================================ 13 关键代码③（新页）
s = new_slide()
page_header(s, "关键代码 ③：一套协议，两种渲染")
page_footer(s, 13)

code3 = """\
/* GUI 客户端：登录时在昵称后面带一个标记 */
snprintf(payload, sizeof payload, "%s\\tgui", nick);
frame_send(g_sock, FRAME_LOGIN, payload, len);

/* 子进程：看到标记就切换成「原样转发」 */
if (type == FRAME_LOGIN) {
    char *tab = strchr(payload, '\\t');
    if (tab && strcmp(tab + 1, "gui") == 0)
        gui_mode = 1;
}

/* 主进程下发内容时，按会话类型走两条路 */
if (gui_mode)
    frame_send(connfd, type, payload, len);  /* 客户端画 */
else
    render_and_send(connfd, type, payload);  /* 服务端画 */"""
code_block(s, L, 1.52, 6.55, 4.35, code3, size=11, title="client.c + server.c · 渲染分流")

tb = textbox(s, 7.78, 1.52, R - 7.78, 4.9)
tf = tb.text_frame
mode_points = [
    ("文本模式（命令行）", "子进程把 SAY / WHISPER / SYS 渲染成带颜色的一行文本，客户端直接打印。终端适配、颜色控制都在服务端做。"),
    ("结构化模式（GUI）", "SAY / WHISPER / SYS / USERS 原样下发，客户端自己决定字号、颜色和列表布局。"),
    ("分流只靠一个标记", "LOGIN 载荷里一个 \\tgui，主进程的路由逻辑一行都不用改。"),
    ("顺带解决列表刷新", "主进程记住哪些会话是 GUI，有人上下线 / 改名时主动推最新 USERS。"),
]
for i, (head, body) in enumerate(mode_points):
    p = para(tf, first=(i == 0), before=0 if i == 0 else 14, line=1.22)
    bullet(p, "▪", GOLD_D, 0.20)
    run(p, head + "：", 11.5, GREEN, BODY_F, bold=True)
    run(p, body, 10.5, BODY, BODY_F)

add_notes(s, """
这里是协议设计上最关键的一个小技巧：同一套帧，两种渲染方式。
命令行客户端不带标记，子进程把消息渲染成"带颜色的一行文本"再发，客户端只负责打印；
图形客户端连接时在 LOGIN 里加一个 \tgui，子进程就把结构化帧原样转发，
由客户端自己画成彩色聊天区。
这样做的好处是：服务端的路由、权限、广播逻辑完全不用改，
客户端想长成什么样都行——这正是"关注点分离"。
另外主进程会记住 GUI 会话，有人上下线时主动推最新在线列表，所以 GUI 的列表能自动刷新。
""")

# ============================================================ 14 可靠性（新页）
s = new_slide()
page_header(s, "四个容易踩的坑")
page_footer(s, 14)

crashes = [
    ("①", "短读短写", "read() 返回值可能小于请求长度",
     "read() 返回的是「这次搬了多少字节」，不是「要了多少」。必须循环补满，否则会把半帧当整帧解析。"),
    ("②", "被信号打断", "read/write 返回 -1，errno = EINTR",
     "信号处理函数执行完打断系统调用是正常现象，判断 errno 后重试即可，不能当作出错直接关连接。"),
    ("③", "SIGPIPE 杀进程", "对端已关闭还继续 write",
     "默认动作是终止进程。用 signal(SIGPIPE, SIG_IGN) 忽略，改为通过 write 返回值判断对端状态。"),
    ("④", "僵尸进程", "子进程退出但父进程没回收",
     "在 SIGCHLD 处理函数里循环 waitpid(-1, NULL, WNOHANG)，把已退出的子进程全部收尸。"),
]
pos = [(L, 1.50), (6.76, 1.50), (L, 3.80), (6.76, 3.80)]
for (num, title, code, body), (x, y) in zip(crashes, pos):
    shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, x, y, 5.62, 2.10, fill=WHITE, line=GREEN, line_w=0.9, radius=0.05)
    shape(s, MSO_SHAPE.RECTANGLE, x, y, 0.07, 2.10, fill=GOLD, line=None)
    tb = textbox(s, x + 0.24, y + 0.16, 0.42, 0.34)
    run(tb.text_frame.paragraphs[0], num, 15, GOLD_D, BODY_F, bold=True)
    tb = textbox(s, x + 0.62, y + 0.17, 4.6, 0.34)
    run(tb.text_frame.paragraphs[0], title, 13.5, GREEN, BODY_F, bold=True)
    tb = textbox(s, x + 0.64, y + 0.60, 4.80, 0.30)
    run(tb.text_frame.paragraphs[0], code, 9.5, RED, MONO)
    tb = textbox(s, x + 0.64, y + 0.94, 4.80, 1.00)
    p = tb.text_frame.paragraphs[0]
    p.line_spacing = 1.22
    run(p, body, 10.5, BODY, BODY_F)

bar = shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, L, 6.12, R - L, 0.56, fill=PANEL_G, line=None, radius=0.12)
tf = bar.text_frame
tf.margin_left = Inches(0.18)
tf.vertical_anchor = MSO_ANCHOR.MIDDLE
run(tf.paragraphs[0], "这四点都不是「功能」，但决定程序能不能在真实网络里稳定跑。", 11.5, GREEN, BODY_F, bold=True)
add_notes(s, """
这四个坑都是我在写这个程序时真实处理过的：
短读短写是网络编程第一坑；
EINTR 是信号引入的坑；
SIGPIPE 最隐蔽，写一个已经断开的 socket 会直接把进程杀掉；
僵尸进程则是 fork 模型必须处理的问题。
每个坑我都写成了固定的封装函数，代码里能直接看到。
""")

# ============================================================ 14 章节 04
sec4 = make_section("04", "演示与可视化", """
最后一部分：跑真实程序，先把命令行版本跑通，再看 Windows 图形客户端和服务端实时看板，
这两个是这次额外做的可视化；最后用自动化测试和并发模型对比收尾。
""", 15)

# ============================================================ 16 演示①（新页）
s = new_slide()
page_header(s, "运行演示：群聊、私聊、在线列表")
page_footer(s, 16)

for title, text, x in [("客户端 A（小明）", CLIENT1, L),
                       ("客户端 B（alice）", CLIENT2, 6.85)]:
    tb = textbox(s, x, 1.24, 5.4, 0.28)
    run(tb.text_frame.paragraphs[0], title, 10.5, GOLD_D, BODY_F, bold=True)
    lines = [ln for ln in text.splitlines() if ln.strip()]
    if len(lines) > 21:
        lines = lines[:21]
    terminal_lines(s, x, 1.54, 5.53, 4.52, lines, size=10.5)

tb = textbox(s, L, 6.20, R - L, 0.60)
tf = tb.text_frame
p = tf.paragraphs[0]
p.line_spacing = 1.2
run(p, "录制方式：", 10, GOLD_D, BODY_F, bold=True)
run(p, "scripts/demo.py 自动启动 1 个服务端和 3 个客户端，按脚本顺序发送消息并抓取各自终端输出；"
       "上面是真实运行结果（已去掉颜色控制符）。", 10, GRAY, BODY_F)
add_notes(s, """
左边是小明的屏幕，右边是 alice 的屏幕，都是真实运行录下来的。
可以看到：
1）有人上线，所有人都会收到系统提示；
2）群聊消息每人一条；
3）alice 发的私聊，只有小明收到，日志里第三个人看不到；
4）/list 返回的在线列表是服务端当场拼的。
注意每条消息前面都有时间和昵称，这是服务端统一渲染后下发的。
""")

# ============================================================ 17 演示②（新页）
s = new_slide()
page_header(s, "运行演示：服务端日志与进程结构")
page_footer(s, 17)

tb = textbox(s, L, 1.24, 6.4, 0.28)
run(tb.text_frame.paragraphs[0], "服务端日志（stderr）", 10.5, GOLD_D, BODY_F, bold=True)
log_lines = [ln for ln in SRV_LOG.splitlines() if ln.strip() and "等待客户端" not in ln]
terminal_lines(s, L, 1.54, 6.45, 3.55, log_lines[:16], size=9.5)

tb = textbox(s, 7.75, 1.24, 4.6, 0.28)
run(tb.text_frame.paragraphs[0], "进程结构（ps -ef 实测）", 10.5, GOLD_D, BODY_F, bold=True)

_ppid_map = {pid: ppid for pid, ppid, cmd in PS_ROWS}
rows = [["PID", "PPID", "角色"],
        [str(MAIN_PID), str(_ppid_map.get(MAIN_PID, 1)), "主进程（accept / 路由）"]]
for pid in CHILD_PIDS:
    rows.append([str(pid), str(MAIN_PID), "子进程（服务一个客户端）"])
grid_table(s, 7.75, 1.54, [1.05, 1.05, 2.53], rows, 0.40, size=10.5, head_size=10.5,
           aligns=[PP_ALIGN.CENTER, PP_ALIGN.CENTER, PP_ALIGN.LEFT])

tb = textbox(s, 7.75, 1.54 + 0.34 + 0.40 * (len(rows) - 1) + 0.16, 4.6, 0.62)
tf = tb.text_frame
p = tf.paragraphs[0]
p.line_spacing = 1.2
run(p, "三个子进程的 PPID 全部指向主进程 —— 这就是「一客户端一进程」。", 10, GREEN, BODY_F, bold=True)

bar = shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, L, 5.42, R - L, 1.15, fill=PANEL, line=None, radius=0.08)
tf = bar.text_frame
tf.margin_left = tf.margin_right = Inches(0.20)
tf.margin_top = Inches(0.12)
p = tf.paragraphs[0]
p.alignment = PP_ALIGN.LEFT
run(p, "日志里每一行「开始为 … 服务」都来自不同的子进程", 11.5, GREEN, BODY_F, bold=True)
for text in ["· 主进程打印「创建子进程 pid=…」后，子进程自己打印「开始为 … 服务」和「完成登录」，所以 pid 交错出现；",
             "· 用户退出时主进程打印「下线」，随后子进程打印「结束」，父子进程通过控制通道的 EOF 协同收尾。"]:
    p2 = tf.add_paragraph()
    p2.space_before = Pt(4)
    p2.line_spacing = 1.15
    p2.alignment = PP_ALIGN.LEFT
    run(p2, text, 10.5, BODY, BODY_F)

add_notes(s, """
左边是服务端日志，右边是同一时刻 ps -ef 抓下来的进程树。
关键证据在右边：三个子进程的 PPID 都是 %d 这个主进程号。
左边日志里"创建子进程 pid=…"是主进程打的，"开始为 … 服务"是子进程自己打的，
所以 pid 交错出现，这正说明它们是不同的进程在并发工作。
""" % MAIN_PID)

# ============================================================ 18 图形客户端（新页）
s = new_slide()
page_header(s, "图形客户端：单文件 exe，可多开")
page_footer(s, 18)

gui_pic = os.path.join(WORK, "gui", "screenshots", "cloud_two_clients.png")
s.shapes.add_picture(gui_pic, Inches(L), Inches(1.42), width=Inches(7.95))
subtitle_note(s, 5.46, "实测：两个客户端窗口同时连到云服务器 119.23.182.147:8888", size=10.5)

tb = textbox(s, 9.15, 1.42, R - 9.15, 4.0)
tf = tb.text_frame
gui_points = [
    ("单文件 · 零依赖", "107 KB 的 exe，双击就跑，不需要 Python / .NET 运行库。"),
    ("多开即多客户端", "开 N 个窗口 = N 个客户端进程，服务端就多 fork 一个子进程。"),
    ("界面不卡", "非阻塞 connect + 独立接收线程，收满一帧再 PostMessage 给界面线程。"),
    ("自己渲染", "群聊 / 私聊 / 系统 / 错误四种配色，右侧在线列表随上下线自动刷新。"),
]
for i, (head, body) in enumerate(gui_points):
    p = para(tf, first=(i == 0), before=0 if i == 0 else 13, line=1.20)
    bullet(p, "▪", GOLD_D, 0.18)
    run(p, head + "：", 11, GREEN, BODY_F, bold=True)
    run(p, body, 9.5, BODY, BODY_F)

bar = shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, L, 5.92, R - L, 0.80, fill=PANEL_G, line=None, radius=0.10)
tf = bar.text_frame
tf.margin_left = Inches(0.22)
tf.margin_right = Inches(0.22)
tf.vertical_anchor = MSO_ANCHOR.MIDDLE
_p = tf.paragraphs[0]
_p.alignment = PP_ALIGN.LEFT
run(_p,
    "GUI 与命令行客户端共用同一套帧协议：GUI 拿结构化帧自己画，命令行收服务端渲染好的文本。",
    11, GREEN, BODY_F, bold=True)

add_notes(s, """
这一页是这次额外做的图形客户端。左边是真实截图：两个窗口同时连到云服务器。
要点讲四个：
1）它是单个 exe 文件，一百多 KB，拷到任何 Windows 机器双击就能跑，不需要装 Python 或 .NET；
2）开几个窗口就是几个客户端进程，服务端就多 fork 一个子进程，正好和"一客户端一进程"对上；
3）连接用非阻塞 connect 加 select 超时，收数据放在独立线程里，所以界面不会卡；
4）它收到的是结构化帧，颜色、字号、列表都自己画，所以比命令行版本好看。
右下角一句话是重点：它和命令行客户端用的是同一套协议，只是渲染方式不同。
""")

# ============================================================ 19 服务端看板（新页）
s = new_slide()
page_header(s, "服务端实时看板：把进程画在屏幕上")
page_footer(s, 19)

dash_pic = os.path.join(CHAT_DOCS, "dashboard_cloud_crop.png")
dash_w = R - L                      # 11.43 in 通栏
dash_h = dash_w * 662 / 1709        # 保持裁剪后的宽高比 ≈ 4.43 in
s.shapes.add_picture(dash_pic, Inches(L), Inches(1.32),
                     width=Inches(dash_w), height=Inches(dash_h))

dash_cards = [
    ("上半屏 · 会话表", "槽位、子进程 PID、昵称、对端地址、在线时长、收发条数"),
    ("下半屏 · 事件流", "fork → 登录 → 群聊广播 → 私聊 → 下线 → waitpid 回收"),
    ("顶部 · 统计", "累计 fork 次数、已回收个数、当前存活子进程数"),
    ("启动方式", "./build/server -p 8888 -d，-d 就有看板"),
]
_card_w = 2.75
_gap = (R - L - _card_w * 4) / 3
for i, (head, body) in enumerate(dash_cards):
    x = L + i * (_card_w + _gap)
    shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, x, 5.86, _card_w, 0.92,
          fill=PANEL_G, line=None, radius=0.10)
    tb = textbox(s, x + 0.14, 5.92, _card_w - 0.26, 0.80)
    run(tb.text_frame.paragraphs[0], head, 10, GREEN_D, BODY_F, bold=True)
    p2 = tb.text_frame.add_paragraph()
    p2.space_before = Pt(2)
    p2.line_spacing = 1.06
    run(p2, body, 8.5, BODY, BODY_F)

add_notes(s, """
这是服务端跑起来的样子，加 -d 参数就会打开这个看板。
上半屏是在线会话表：每一行是一个会话，第二列就是服务端为它 fork 出来的子进程 PID，
所以"一个客户端一个进程"在这张表里是看得见、数得出来的。
下半屏是事件流：谁 fork 了、谁登录了、群聊广播给了几个人、谁和谁私聊、谁下线、谁被 waitpid 回收。
顶上是统计：累计 fork 了多少次、回收了多少、当前还活着几个。
演示的时候我一边操作客户端，这一屏就会跟着变，比滚日志直观得多。
""")

# ============================================================ 20 看板原理（新页）
s = new_slide()
page_header(s, "看板原理：整屏重绘，为什么不会滚")
page_footer(s, 20)

code20 = r'''/* 每行结尾清到行尾：短行不会留残影 */
"%s%s\033[K\r\n"

/* 每帧最后一行不带换行：刚好占满行数 */
if (buf[len-2]=='\r' && buf[len-1]=='\n')
    len -= 2;

/* 行数、列数每帧按终端实测 */
ioctl(1, TIOCGWINSZ, &ws);'''
code_block(s, L, 1.52, 6.30, 2.85, code20, size=11, title="dash.c · 一帧的渲染规则")

tb = textbox(s, 7.60, 1.52, R - 7.60, 2.9)
tf = tb.text_frame
draw_points = [
    ("全屏重绘", "每帧 ESC[H 回到左上角逐行覆盖，不需要记住每个字段的位置。"),
    ("环形缓冲", "事件最多留 64 条；主循环 select 超时 0.5 秒，重画一帧。"),
    ("中文对齐", "按「显示宽度」算格子（中文占 2 格），表格才不会错位。"),
    ("互不干扰", "看板模式下子进程 stderr 重定向到 /dev/null，日志不冲界面。"),
]
for i, (head, body) in enumerate(draw_points):
    p = para(tf, first=(i == 0), before=0 if i == 0 else 12, line=1.20)
    bullet(p, "▪", GOLD_D, 0.18)
    run(p, head + "：", 11, GREEN, BODY_F, bold=True)
    run(p, body, 9.5, BODY, BODY_F)

bar = shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, L, 4.70, R - L, 1.95, fill=PANEL_R, line=None, radius=0.08)
tf = bar.text_frame
tf.vertical_anchor = MSO_ANCHOR.MIDDLE
tf.margin_left = tf.margin_right = Inches(0.22)
tf.margin_top = Inches(0.14)
p = tf.paragraphs[0]
p.alignment = PP_ALIGN.LEFT
run(p, "踩坑记录：最后一行也换行 → 整屏上滚", 11.5, RED, BODY_F, bold=True)
for text in ["一帧正好画满终端行数，如果最后一行也带 \\r\\n，光标会越过屏幕底部，终端就上滚一行；"
             "下一帧 ESC[H 回到的是滚动后的顶部，于是表现为「最上面一行一直跳、底部提示语不断堆叠」。",
             "修法就是上面三条：最后一行不换行、每行清到行尾、行数严格等于终端真实高度。"]:
    p2 = tf.add_paragraph()
    p2.space_before = Pt(5)
    p2.line_spacing = 1.20
    p2.alignment = PP_ALIGN.LEFT
    run(p2, text, 10, BODY, BODY_F)

add_notes(s, """
这一页讲看板是怎么画出来的，也讲我踩的坑。
思路很简单：整屏重绘。每帧把光标移回左上角，一行一行覆盖，上一帧的内容自然被盖掉。
数据来源是主进程维护的用户表和事件环形缓冲，主循环 select 超时 0.5 秒就重画一帧。
刷新最早的版本有两个毛病：一是每行没有清到行尾，短行盖不住旧的长行，会出现"在线 在线"重复；
二是最后一行也写了换行，光标越过屏幕底部导致整屏上滚，看起来就是顶部行一直跳、底部提示一行行堆。
修法就三条：最后一行不换行、每行清到行尾、行数严格等于终端真实高度。
现在 8 行到 50 行的终端都验证过，每帧正好占满、不会滚。
""")

# ============================================================ 21 测试（新页）
s = new_slide()
page_header(s, "自动化测试：12 项协议断言")
page_footer(s, 21)

tests = [
    "客户端登录并收到欢迎语", "新用户上线广播通知",
    "重复昵称被拒绝", "群聊消息广播到 B", "群聊消息广播到 C",
    "私聊送达目标用户", "私聊不泄露给第三方", "在线列表包含全部昵称",
    "非法昵称被拒绝", "退出时收到 BYE 帧", "其他用户收到离开通知",
    "退出后仍可接入新连接",
]
col_x = [L, 6.62]
for i, text in enumerate(tests):
    col = 0 if i < 6 else 1
    row = i if i < 6 else i - 6
    x = col_x[col]
    y = 1.62 + row * 0.62
    shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, x, y, 5.55, 0.50, fill=PANEL_G, line=None, radius=0.16)
    tb = textbox(s, x + 0.18, y, 0.34, 0.50, anchor=MSO_ANCHOR.MIDDLE)
    run(tb.text_frame.paragraphs[0], "✓", 13, GREEN, BODY_F, bold=True)
    tb = textbox(s, x + 0.56, y, 4.85, 0.50, anchor=MSO_ANCHOR.MIDDLE)
    run(tb.text_frame.paragraphs[0], text, 11.5, BODY, BODY_F)

score = shape(s, MSO_SHAPE.ROUNDED_RECTANGLE, L, 5.70, 2.6, 0.86, fill=GREEN, line=None, radius=0.10)
tf = score.text_frame
tf.vertical_anchor = MSO_ANCHOR.MIDDLE
p = tf.paragraphs[0]
p.alignment = PP_ALIGN.CENTER
run(p, "12 / 12 通过", 20, GOLD, BODY_F, bold=True)

tb = textbox(s, 3.72, 5.82, R - 3.72, 0.66)
p = tb.text_frame.paragraphs[0]
p.line_spacing = 1.2
run(p, "python3 scripts/smoke_test.py", 11, GREEN_D, MONO)
p2 = tb.text_frame.add_paragraph()
run(p2, "直接用 socket 按帧格式与服务端对话，逐条断言协议行为，不依赖人工点击。", 10, GRAY, BODY_F)

add_notes(s, """
光"能跑"不够，所以我又写了一个协议级的自动化测试：
用 Python 直接按帧格式连服务端，覆盖 12 项行为断言，包括查重、广播、私聊隔离、
退出通知、非法昵称、连接数上限等等。
当前结果 12/12 全部通过。这也是 PPT 里唯一一个"可复现"的实验证据。
""")

# ============================================================ 22 对比总结（新页）
s = new_slide()
page_header(s, "三种并发模型的取舍")
page_footer(s, 22)

rows = [["模型", "并发单位", "优势", "代价"],
        ["多进程（本项目）", "进程", "隔离彻底，共享状态少，不用加锁", "创建/切换开销大，跨进程要 IPC"],
        ["多线程", "线程", "轻量，共享内存方便，编程直观", "要加锁，一处崩溃可能拖垮整个进程"],
        ["单进程 select", "事件", "无并发开销，适合海量连接", "难以利用多核，编码复杂"]]
grid_table(s, L, 1.52, [2.35, 1.35, 3.95, 3.78], rows, 0.82, size=11, head_size=11.5,
           aligns=[PP_ALIGN.LEFT] * 4, head_h=0.40)

tb = textbox(s, L, 4.70, R - L, 1.45)
tf = tb.text_frame
p = tf.paragraphs[0]
run(p, "结论", 13, GREEN, BODY_F, bold=True)
for text in ["本项目选择多进程：正好覆盖课程里的进程创建、进程间通信、信号与进程回收；",
             "工程上更常见的是「多线程 + epoll」（如主流聊天服务器），本质是把 fork 换成线程、把 select 换成 epoll；",
             "理解了进程模型，换成线程模型只是把并发单位从进程换成线程，通信方式从 socketpair 换成共享内存。"]:
    p2 = tf.add_paragraph()
    p2.space_before = Pt(7)
    p2.line_spacing = 1.22
    bullet(p2, "▪", GOLD_D, 0.22)
    run(p2, text, 11.5, BODY, BODY_F)

add_notes(s, """
最后做个对比：
多进程隔离最好，代价是 IPC 和创建开销；
多线程轻量好写，代价是锁和稳定性；
单进程 select 适合大量连接，但很难利用多核。
我们这个题目的要求是"每个客户端一个进程"，所以选多进程；
但思路是相通的：并发单位可以换，调度和回收的问题不会消失。
""")

# ============================================================ 19 结束页
# 模板这句 "Thanks for Watching" 用的是缺失的装饰字体（会渲染成希腊字母状），换成通用字体
thanks_sub = find_shape(thanks, "文本框 8")
for p in thanks_sub.text_frame.paragraphs:
    for r in p.runs:
        r.font.name = BODY_F
        r.font.size = Pt(18)
        _set_ea(r, BODY_F)
thanks_sub.width = Inches(5.0)

tb = textbox(thanks, 1.25, 5.62, 11.0, 0.40)
run(tb.text_frame.paragraphs[0],
    "演示代码：chat/server.c · chat/src/dash.c · gui/chat_gui.c · chat/scripts/ · chat/docs/",
    10.5, GRAY, BODY_F)
add_notes(thanks, """
我的展示到这里，谢谢大家。
代码在 chat 目录：server.c 是服务端，dash.c 是实时看板，client.c 是命令行客户端；
gui 目录是 Windows 图形客户端和它的源码；scripts 下是自动演示和协议测试脚本；
docs 里是这次录制的原始输出和截图，都可以现场打开验证。
""")

# ============================================================ 排序并保存
# 新页都建好了，现在删掉模板里的示例页
for s in trash:
    delete_slide(prs, s)

known = {x.part.partname for x in (cover, toc, sec_tpl, six, thanks, sec2, sec3, sec4)}
new_pages = [sl for sl in prs.slides if sl.part.partname not in known]
# new_pages 顺序 = 创建顺序：arch, roles, lifecycle, protocol, code1, code2, code3,
#                            reliability, demo1, demo2, gui, dash, principle, tests, compare
(arch, roles, lifecycle, protocol, code1s, code2s, code3s, reliability,
 demo1, demo2, gui_s, dash_s, principle, tests_s, compare) = new_pages

ordered = [cover, toc, sec_tpl, six, arch, sec2, roles, lifecycle, sec3, protocol,
           code1s, code2s, code3s, reliability, sec4, demo1, demo2,
           gui_s, dash_s, principle, tests_s, compare, thanks]
reorder(prs, ordered)

prs.save(OUTPUT)
print("已生成：%s" % OUTPUT)
print("页数：%d" % len(prs.slides))
