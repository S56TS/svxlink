"""Build the MQTT guide as DOCX using Python 3's standard library.

Run from any directory: python3 docs/build_mqtt_docx.py
Supports the headings, paragraphs, fenced code, lists and tables in this guide.
"""
from pathlib import Path
import re
from xml.sax.saxutils import escape
from zipfile import ZipFile, ZIP_DEFLATED
import xml.etree.ElementTree as ET

BASE = Path(__file__).resolve().parent
SOURCE = BASE / 'MQTT_TELEMETRY_AND_SERVER.md'
OUTPUT = SOURCE.with_suffix('.docx')
W = 'http://schemas.openxmlformats.org/wordprocessingml/2006/main'

def runs(text, code=False):
    parts = [text] if code else re.split(r'(`[^`]+`)', text)
    result = ''
    for part in parts:
        mono = code or (part.startswith('`') and part.endswith('`'))
        if mono and not code:
            part = part[1:-1]
        prop = '<w:rPr><w:rFonts w:ascii="Consolas" w:hAnsi="Consolas"/><w:sz w:val="18"/></w:rPr>' if mono else ''
        result += '<w:r>' + prop + '<w:t xml:space="preserve">' + escape(part) + '</w:t></w:r>'
    return result

def paragraph(text, style='Normal', code=False):
    return f'<w:p><w:pPr><w:pStyle w:val="{style}"/></w:pPr>{runs(text, code)}</w:p>'

lines = SOURCE.read_text(encoding='utf-8').splitlines()
body = []
i = 0
while i < len(lines):
    line = lines[i]
    if not line.strip():
        i += 1
        continue
    if line.startswith('```'):
        i += 1
        while i < len(lines) and not lines[i].startswith('```'):
            body.append(paragraph(lines[i], 'Code', True))
            i += 1
        i += 1
        continue
    if line.startswith('|'):
        rows = []
        while i < len(lines) and lines[i].startswith('|'):
            cells = [x.strip() for x in lines[i].strip('|').split('|')]
            if not all(re.fullmatch(r':?-+:?', x) for x in cells):
                rows.append(cells)
            i += 1
        n = len(rows[0])
        widths = ([2600, 550, 800, 5688] if n == 4 else
                  [3300, 6338] if n == 2 else [3100, 2600, 3938])
        table = '<w:tbl><w:tblPr><w:tblW w:w="9638" w:type="dxa"/><w:tblLayout w:type="fixed"/><w:tblBorders>'
        table += ''.join(f'<w:{edge} w:val="single" w:sz="4" w:color="CED7E0"/>' for edge in ['top','left','bottom','right','insideH','insideV'])
        table += '</w:tblBorders></w:tblPr><w:tblGrid>'
        table += ''.join(f'<w:gridCol w:w="{width}"/>' for width in widths) + '</w:tblGrid>'
        for index, row in enumerate(rows):
            table += '<w:tr><w:trPr><w:cantSplit/>' + ('<w:tblHeader/>' if index == 0 else '') + '</w:trPr>'
            for col, cell in enumerate(row):
                shade = '<w:shd w:fill="E7EEF5"/>' if index == 0 else ''
                table += f'<w:tc><w:tcPr><w:tcW w:w="{widths[col]}" w:type="dxa"/>{shade}</w:tcPr>{paragraph(cell, "TableText")}</w:tc>'
            table += '</w:tr>'
        body.append(table + '</w:tbl>')
        continue
    heading = re.match(r'^(#{1,3}) (.*)', line)
    if heading:
        level = len(heading[1])
        body.append(paragraph(heading[2], 'Title' if level == 1 else f'Heading{level-1}'))
        i += 1
        continue
    list_item = re.match(r'^(?:- |\d+\. )', line)
    text = line
    i += 1
    while i < len(lines) and lines[i].strip() and not re.match(r'^(#|\||```|- |\d+\. )', lines[i]):
        text += ' ' + lines[i].strip()
        i += 1
    if text.startswith('- '):
        text = '\u2022 ' + text[2:]
    body.append(paragraph(text, 'List' if list_item else 'Normal'))

styles = f'''<w:styles xmlns:w="{W}">
<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii="Calibri" w:hAnsi="Calibri"/><w:sz w:val="21"/></w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:after="120" w:line="260" w:lineRule="auto"/></w:pPr></w:pPrDefault></w:docDefaults>
<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/></w:style>
<w:style w:type="paragraph" w:styleId="Title"><w:name w:val="Title"/><w:pPr><w:keepNext/><w:spacing w:after="260"/></w:pPr><w:rPr><w:b/><w:color w:val="17365D"/><w:sz w:val="40"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/><w:pPr><w:keepNext/><w:spacing w:before="260" w:after="140"/><w:outlineLvl w:val="0"/></w:pPr><w:rPr><w:b/><w:color w:val="17365D"/><w:sz w:val="29"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Heading2"><w:name w:val="heading 2"/><w:pPr><w:keepNext/><w:spacing w:before="180"/><w:outlineLvl w:val="1"/></w:pPr><w:rPr><w:b/><w:sz w:val="24"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Code"><w:name w:val="Code"/><w:pPr><w:spacing w:after="0" w:line="210"/><w:shd w:fill="F3F5F7"/></w:pPr></w:style>
<w:style w:type="paragraph" w:styleId="List"><w:name w:val="List"/><w:pPr><w:ind w:left="240" w:hanging="240"/></w:pPr></w:style>
<w:style w:type="paragraph" w:styleId="TableText"><w:name w:val="Table Text"/><w:pPr><w:spacing w:before="60" w:after="60" w:line="230"/></w:pPr><w:rPr><w:sz w:val="18"/></w:rPr></w:style>
</w:styles>'''
document = f'<w:document xmlns:w="{W}" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><w:body>' + ''.join(body) + '<w:sectPr><w:footerReference w:type="default" r:id="rId2"/><w:pgSz w:w="11906" w:h="16838"/><w:pgMar w:top="1134" w:right="1134" w:bottom="1134" w:left="1134" w:header="567" w:footer="567"/></w:sectPr></w:body></w:document>'
types = '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/>' + ''.join(f'<Override PartName="/word/{name}.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.{kind}+xml"/>' for name, kind in [('document','document.main'),('styles','styles'),('footer1','footer')]) + '</Types>'
rels_ns = 'http://schemas.openxmlformats.org/package/2006/relationships'
rel_type = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships/'
parts = {
    '[Content_Types].xml': types,
    '_rels/.rels': f'<Relationships xmlns="{rels_ns}"><Relationship Id="rId1" Type="{rel_type}officeDocument" Target="word/document.xml"/></Relationships>',
    'word/document.xml': document,
    'word/styles.xml': styles,
    'word/_rels/document.xml.rels': f'<Relationships xmlns="{rels_ns}"><Relationship Id="rId1" Type="{rel_type}styles" Target="styles.xml"/><Relationship Id="rId2" Type="{rel_type}footer" Target="footer1.xml"/></Relationships>',
    'word/footer1.xml': f'<w:ftr xmlns:w="{W}"><w:p><w:pPr><w:jc w:val="center"/></w:pPr>{runs("AIORS MQTT | ")}<w:fldSimple w:instr="PAGE"/></w:p></w:ftr>',
}
with ZipFile(OUTPUT, 'w', ZIP_DEFLATED) as z:
    for name, xml in parts.items():
        ET.fromstring(xml)
        z.writestr(name, '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>' + xml)
with ZipFile(OUTPUT) as z:
    assert z.testzip() is None
    tree = ET.fromstring(z.read('word/document.xml'))
    assert len(tree.findall(f'.//{{{W}}}tbl')) == 4
print(f'Created and validated {OUTPUT} ({OUTPUT.stat().st_size} bytes)')
