from reportlab.lib.pagesizes import A4
from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.units import cm
from reportlab.platypus import SimpleDocTemplate, Table, TableStyle, Paragraph, Spacer
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
import os

# Try to register a CJK font for Chinese characters
font_paths = [
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJKtc-Regular.otf",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/arphic/uming.ttc",
]

cjk_font = "Helvetica"
for fp in font_paths:
    if os.path.exists(fp):
        try:
            pdfmetrics.registerFont(TTFont("CJK", fp))
            cjk_font = "CJK"
            break
        except Exception:
            continue

doc = SimpleDocTemplate(
    "/home/user/ai-depth-plugin/AIDepthMap_Manual.pdf",
    pagesize=A4,
    rightMargin=2*cm, leftMargin=2*cm,
    topMargin=2*cm, bottomMargin=2*cm,
)

styles = getSampleStyleSheet()
title_style = ParagraphStyle("title", fontName=cjk_font, fontSize=20, spaceAfter=8, textColor=colors.HexColor("#1a1a2e"))
h2_style    = ParagraphStyle("h2",    fontName=cjk_font, fontSize=13, spaceAfter=6, spaceBefore=14, textColor=colors.HexColor("#16213e"))
body_style  = ParagraphStyle("body",  fontName=cjk_font, fontSize=10, spaceAfter=4)
cell_style  = ParagraphStyle("cell",  fontName=cjk_font, fontSize=9,  leading=13)

def cell(text):
    return Paragraph(text, cell_style)

header_bg   = colors.HexColor("#1a1a2e")
row_even_bg = colors.HexColor("#f0f4ff")
row_odd_bg  = colors.white

story = []

# ── Feature Table ──────────────────────────────────────────────────────────
story.append(Paragraph("功能介紹", h2_style))

features = [
    ("Quality",       "推理品質與效能權衡，低值跳幀省資源，高值每幀都推理"),
    ("Smooth",        "時間平滑強度，高值讓深度圖較穩定不閃爍"),
    ("Invert",        "翻轉深度方向，使近處變暗、遠處變亮"),
    ("Depth Near",    "可視深度範圍的近端起點"),
    ("Depth Far",     "可視深度範圍的遠端終點"),
    ("Sweep",         "開啟深度切片，以亮帶標記特定深度層"),
    ("Sweep Dir",     "亮帶掃描方向（近→遠 或 遠→近）"),
    ("Sweep Pos",     "亮帶目前位置，可與 BPM 同步驅動"),
    ("Scan Line",     "切換 Z-Depth 灰階模式與掃描線特效模式"),
    ("SL Effect",     "掃描線樣式：水平線 / 深度扭曲線 / 同心圓 / LED 點陣"),
    ("SL Density",    "掃描線密度或點陣格數"),
    ("SL Width",      "線條粗細或點的半徑"),
    ("SL Warp",       "線條依深度偏移的程度（扭曲線專用）"),
    ("SL Offset",     "掃描線捲動位置，可與 BPM 同步驅動"),
    ("SL Blend Vid",  "特效與原始影像的混合比例"),
    ("Particle",      "開啟粒子發光模式，沿輪廓邊緣生成發光粒子"),
    ("P Density",     "粒子格點數量，控制粒子分布密度"),
    ("P Size",        "粒子大小（近處較大、遠處較小）"),
    ("P Glow",        "粒子發光亮度"),
    ("P Drift",       "粒子向外擴散的速度與距離"),
]

feat_data = [[cell("功能名稱"), cell("說明")]]
for name, desc in features:
    feat_data.append([cell(name), cell(desc)])

feat_table = Table(feat_data, colWidths=[4*cm, 13*cm])
feat_table.setStyle(TableStyle([
    ("BACKGROUND",  (0,0), (-1,0),  header_bg),
    ("TEXTCOLOR",   (0,0), (-1,0),  colors.white),
    ("FONTNAME",    (0,0), (-1,-1), cjk_font),
    ("FONTSIZE",    (0,0), (-1,-1), 9),
    ("ROWBACKGROUNDS", (0,1), (-1,-1), [row_even_bg, row_odd_bg]),
    ("GRID",        (0,0), (-1,-1), 0.4, colors.HexColor("#c0c8e0")),
    ("VALIGN",      (0,0), (-1,-1), "MIDDLE"),
    ("TOPPADDING",  (0,0), (-1,-1), 5),
    ("BOTTOMPADDING",(0,0),(-1,-1), 5),
    ("LEFTPADDING", (0,0), (-1,-1), 6),
]))
story.append(feat_table)
story.append(Spacer(1, 0.5*cm))

# ── Effect Types ───────────────────────────────────────────────────────────
story.append(Paragraph("SL Effect 樣式速查", h2_style))

effect_data = [
    [cell("Effect 值"), cell("樣式名稱"), cell("說明")],
    [cell("0.00 – 0.33"), cell("水平掃描線"), cell("固定間距水平線，密度與寬度可調")],
    [cell("0.33 – 0.66"), cell("深度扭曲線"), cell("線條依深度值偏移，產生波浪視覺")],
    [cell("0.66 – 0.99"), cell("同心圓線"),   cell("從畫面中心向外放射的環形線條")],
    [cell("~ 1.00"),      cell("LED 點陣"),   cell("格點發光，顏色從原始影像取樣")],
]

effect_table = Table(effect_data, colWidths=[3.5*cm, 3.5*cm, 10*cm])
effect_table.setStyle(TableStyle([
    ("BACKGROUND",  (0,0), (-1,0),  header_bg),
    ("TEXTCOLOR",   (0,0), (-1,0),  colors.white),
    ("FONTNAME",    (0,0), (-1,-1), cjk_font),
    ("FONTSIZE",    (0,0), (-1,-1), 9),
    ("ROWBACKGROUNDS", (0,1), (-1,-1), [row_even_bg, row_odd_bg]),
    ("GRID",        (0,0), (-1,-1), 0.4, colors.HexColor("#c0c8e0")),
    ("VALIGN",      (0,0), (-1,-1), "MIDDLE"),
    ("TOPPADDING",  (0,0), (-1,-1), 5),
    ("BOTTOMPADDING",(0,0),(-1,-1), 5),
    ("LEFTPADDING", (0,0), (-1,-1), 6),
]))
story.append(effect_table)

doc.build(story)
print("PDF generated: AIDepthMap_Manual.pdf")
