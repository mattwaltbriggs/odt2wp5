/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* odt2wp5 / WP4Generator implementation.
 *
 * Emits a WordPerfect 4.0 for Macintosh document from the neutral RVNG event
 * stream, following the "Macintosh WordPerfect 4.0 File Format Manual"
 * (Corel, 22 Apr 93 revision). Function layouts verified against the manual:
 *
 *   fixed  $C0  ext char      length 5   <$C0><Mac><set><char><$C0>
 *   fixed  $C1  tab           length 8   <$C1><def><mode/type>{adjust}<$C1>
 *   fixed  $C2  indent        length 7   <$C2><def>{margin adjust}<$C2>
 *   fixed  $C3  attribute     length 4   <$C3><attr><state><$C3>
 *   var    $D0/$01 hor margins Length 20  {oldL}{oldR}{newL}{newR} (PX)
 *   var    $D0/$02 line spacing Length 12 {old}{new} (PX ratio)
 *   var    $D0/$05 vert margins Length 20 {oldT}{oldB}{newT}{newB} (PX)
 *   var    $D0/$06 justification Length 6 <old><new> (0=L 1=C 2=R 3=Full)
 *   var    $D0/$08 page # pos  Length 6   <old><new> (0-8)
 *   var    $D1/$00 text color  Length 16  [old RGB 6][new RGB 6] (16-bit comps)
 *   var    $D1/$01 text font   Variable   {stamp}[old#][oldT][new#][newT]+Pascal name
 *   var    $D1/$02 font size   Length 8   [old][new] (integer points)
 *   var    $D2/$02 par # defs  Length 52  8 defs .8 + 8 levels .8, old and new
 *   var    $D3/$02 ftn # seed  Length 8   [old][new]
 *   var    $D3/$03 enn # seed  Length 8   [old][new]
 *   var    $D5/$00-03 headers  Variable   flags,{stamp},{ptr},oldDef,{oldH},
 *                                     [oldLen],newDef,{newH},[newLen],<text>
 *   var    $D6/$00 footnote    Variable   flags,{stamp},{ptr},[win#],{sizes},<text>
 *   var    $D6/$01 endnote     Variable   same-sized block of zeros, <text>
 *   var    $D8/$01 insert par# Length 21  <level> + 8 level-counter words
 *   var    $E2/$01 table def   Variable   stamp,ID1,ID2,borders,colors,mode,gutters,
 *                                     rows,cols, 10 bytes per column
 *   var    $E2/$02 cell span   Length 6   <h spanned><v spanned> (0 = none)
 *   EOL    $DC/<type> short form:      <$DC><type>[$0002]<type><$DC>
 *                                     $02 hard return, $07 hard page,
 *                                     $16 end of cell, $18 end of row,
 *                                     $1A end of row/end of table
 *
 * Framing of variable-length groups (verified against the manual's fixed
 * Length values, e.g. $D0/$01 = 20 = 16-byte payload + 4): <$grp><$sub>[Length]
 * [payload][Length]<$sub><$grp>, with Length = payload size + 4.
 */

#include "WP4Generator.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>

#include "WP5CharMap.inc" // Unicode -> WP5.1 (charset, character) reverse map
#include "FontTypeface.h" // shared ODT font-family classifier

namespace
{
const unsigned char W4_EXT_CHAR    = 0xC0;
const unsigned char W4_TAB_GROUP   = 0xC1; // /00 tab
const unsigned char W4_INDENT_GROUP = 0xC2; // /00 indent, /01 left+right
const unsigned char W4_ATTRIBUTE   = 0xC3;
const unsigned char W4_PAGE_FORMAT = 0xD0;
const unsigned char W4_FONT_GROUP  = 0xD1;
const unsigned char W4_DEF_GROUP   = 0xD2;
const unsigned char W4_SET_GROUP   = 0xD3;
const unsigned char W4_HEADER_FOOTER_GROUP = 0xD5;
const unsigned char W4_NOTE_GROUP  = 0xD6;
const unsigned char W4_DISPLAY_GROUP = 0xD8;
const unsigned char W4_TABLE_GROUP = 0xE2;
const unsigned char W4_EOL_GROUP   = 0xDC;

// $D0 = Page Format Group sub-codes
const unsigned char W4_SUB_HORZ_MARGINS  = 0x01;
const unsigned char W4_SUB_LINE_SPACING  = 0x02;
const unsigned char W4_SUB_VERT_MARGINS  = 0x05;
const unsigned char W4_SUB_JUSTIFICATION = 0x06;
const unsigned char W4_SUB_PAGE_NUM_POS  = 0x08;

// $D1 = Font Group sub-codes
const unsigned char W4_SUB_TEXT_COLOR = 0x00;
const unsigned char W4_SUB_TEXT_FONT  = 0x01;
const unsigned char W4_SUB_FONT_SIZE  = 0x02;

// $D2 Definition Group + $D3 Set Group sub-codes
const unsigned char W4_SUB_PAR_NUM_DEF = 0x02;
const unsigned char W4_SUB_SET_FTN_NUM = 0x02;
const unsigned char W4_SUB_SET_ENN_NUM = 0x03;

// $D5 Header/Footer Group + $D6 Footnote/Endnote Group sub-codes
const unsigned char W4_SUB_HEADER_A = 0x00;
const unsigned char W4_SUB_FOOTER_A = 0x02;
const unsigned char W4_SUB_FOOTNOTE = 0x00;
const unsigned char W4_SUB_ENDNOTE  = 0x01;

// $D8 Display Group + $E2 Tables Group sub-codes
const unsigned char W4_SUB_INSERT_PAR_NUM = 0x01;
const unsigned char W4_SUB_TABLE_DEF = 0x01;
const unsigned char W4_SUB_CELL_SPAN = 0x02;

// $DC End-of-Line/Page Group types (short form <$DC><type>[$0002]<type><$DC>)
const unsigned char W4_EOL_HARD_RETURN = 0x02; // HardEOL_
const unsigned char W4_EOL_HARD_PAGE   = 0x07; // HardEOP_
const unsigned char W4_EOL_TABLE_CELL  = 0x16; // TableCell_
const unsigned char W4_EOL_TABLE_ROW   = 0x18; // TableRow_
const unsigned char W4_EOL_TABLE_OFF   = 0x1A; // TableOff_

// $C3 attribute codes: index into the formatter status variable
// (0 bold, 1 italics, 2 underline, 9 strike out, 10 sub, 11 super, 12 double underline)
const unsigned char W4_ATTR_BOLD             = 0;
const unsigned char W4_ATTR_ITALICS          = 1;
const unsigned char W4_ATTR_UNDERLINE        = 2;
const unsigned char W4_ATTR_STRIKE_OUT       = 9;
const unsigned char W4_ATTR_SUBSCRIPT        = 10;
const unsigned char W4_ATTR_SUPERSCRIPT      = 11;
const unsigned char W4_ATTR_DOUBLE_UNDERLINE = 12;

// Single-byte functions (self-contained gates)
const unsigned char W4_HARD_SPACE        = 0xA0;
const unsigned char W4_FOOTNOTE_NUM_BYTE = 0xA2; // current footnote number
const unsigned char W4_ENDNOTE_NUM_BYTE  = 0xA4; // current endnote number
const unsigned char W4_PAR_GATE_ON  = 0xAA;
const unsigned char W4_PAR_GATE_OFF = 0xAB;
} // namespace

// ---- little-endian + PX helpers --------------------------------------------

void WP4Generator::put16(std::vector<unsigned char> &v, unsigned value)
{
	v.push_back((unsigned char)(value & 0xff));
	v.push_back((unsigned char)((value >> 8) & 0xff));
}

void WP4Generator::put32(std::vector<unsigned char> &v, unsigned long value)
{
	v.push_back((unsigned char)(value & 0xff));
	v.push_back((unsigned char)((value >> 8) & 0xff));
	v.push_back((unsigned char)((value >> 16) & 0xff));
	v.push_back((unsigned char)((value >> 24) & 0xff));
}

// inches -> 72dpi fixed-point pixels (WP4's absolute unit; the manual's tab
// example uses 1/2 inch = $00240000, i.e. 0.5 * 72 * 65536).
unsigned long WP4Generator::px(double inches)
{
	double v = inches * 72.0 * 65536.0;
	if (v < 0) v = 0;
	return (unsigned long)(v + 0.5);
}

// ---- construction / document events ----------------------------------------

WP4Generator::WP4Generator(const char *outputFileName) :
	m_parGateOpen(false),
	m_savedParGateOpen(false),
	m_outputFileName(outputFileName ? outputFileName : ""),
	m_body(),
	m_subDoc(),
	m_out(&m_body),
	m_hfSubGroup(0),
	m_hfHasPageNumber(false),
	m_savedFont{3, "Geneva"},
	m_savedFontSize(12),
	m_savedColor(0),
	m_savedJustification(0),
	m_savedSpacing(0x00010000),
	m_savedAttributeStack(),
	m_noteIsEndnote(false),
	m_noteNumber(0),
	m_footnoteCounter(0),
	m_endnoteCounter(0),
	m_noteFirstParaPending(false),
	m_skipNextTab(false),
	m_inCell(false),
	m_currentColumn(0),
	m_tableNumColumns(0),
	m_tableNumRows(1),
	m_tableRowCount(0),
	m_tableOffDone(true),
	m_tableIdCounter(0),
	m_listLevel(-1),
	m_outlineOn(false),
	m_listOrdered(),
	m_listFormat(),
	m_listSuffix(),
	m_listPrefix(),
	m_listCounter(),
	m_pendingBullet(false),
	m_pendingNumberSpace(false),
	m_pendingIndentInches(0.0),
	m_currentFont{3, "Geneva"},   // Mac WP4 default document font
	m_currentFontSize(12),
	m_currentColor(0),
	m_fontStack(),
	m_fontSizeStack(),
	m_colorStack(),
	m_attributeStack(),
	m_pageNumPos(6),              // WP4 default page #: bottom center
	m_currentJustification(0),    // WP4: 0 = left
	m_currentSpacing(0x00010000), // single spacing
	m_marginLeftT(1.0), m_marginRightT(1.0),
	m_marginTopT(1.0), m_marginBottomT(1.0),
	m_marginLeft(1.0), m_marginRight(1.0),
	m_marginTop(1.0), m_marginBottom(1.0),
	m_marginDirty(false)
{
	for (int i = 0; i < 8; i++)
	{
		m_listOrdered[i] = true;
		m_listFormat[i] = '1';
		m_listSuffix[i] = ".";
		m_listPrefix[i].clear();
		m_listCounter[i] = 0;
	}
}

WP4Generator::~WP4Generator()
{
}

void WP4Generator::startDocument(const librevenge::RVNGPropertyList & /*propList*/)
{
	m_body.clear();
	m_subDoc.clear();
	m_out = &m_body;
	m_hfSubGroup = 0;
	m_hfHasPageNumber = false;
	m_noteIsEndnote = false;
	m_noteNumber = 0;
	m_footnoteCounter = 0;
	m_endnoteCounter = 0;
	m_noteFirstParaPending = false;
	m_skipNextTab = false;
	m_inCell = false;
	m_currentColumn = 0;
	m_tableNumColumns = 0;
	m_tableNumRows = 1;
	m_tableRowCount = 0;
	m_tableOffDone = true;
	m_tableIdCounter = 0;
	m_listLevel = -1;
	m_outlineOn = false;
	m_pendingBullet = false;
	m_pendingNumberSpace = false;
	m_pendingIndentInches = 0.0;
	for (int i = 0; i < 8; i++)
	{
		m_listOrdered[i] = true;
		m_listFormat[i] = '1';
		m_listSuffix[i] = ".";
		m_listPrefix[i].clear();
		m_listCounter[i] = 0;
	}
	m_currentFont = MacFont{3, "Geneva"};
	m_currentFontSize = 12;
	m_currentColor = 0;
	m_fontStack.clear();
	m_fontSizeStack.clear();
	m_colorStack.clear();
	m_attributeStack.clear();
	m_pageNumPos = 6;
	m_currentJustification = 0;
	m_currentSpacing = 0x00010000;
	m_marginLeftT = m_marginRightT = m_marginTopT = m_marginBottomT = 1.0;
	m_marginLeft = m_marginRight = m_marginTop = m_marginBottom = 1.0;
	m_marginDirty = false;
	m_parGateOpen = false;
	m_savedParGateOpen = false;
}

void WP4Generator::endDocument()
{
	flushParGate();

	// 16-byte WordPerfect prefix (manual, "WordPerfect Prefix"): 4-byte File
	// ID, 4-byte pointer to the document area (little-endian, per the manual
	// footnote), product code pfwp=1, file type pfwpdoc=10, major/minor
	// version 4.0, 2-byte encryption key (none), 2 bytes reserved.
	//
	// No embedded Macintosh resource block: the manual's "Embedded
	// Macintosh Resource Fork" section makes it optional (attached resource
	// fork is normally transported in the Mac file's own second fork), so
	// the document pointer is simply 16.
	std::vector<unsigned char> file;
	file.push_back(0xFF);
	file.push_back('W');
	file.push_back('P');
	file.push_back('C');
	put32(file, 16);        // document area starts right after the prefix
	file.push_back(0x01);   // product code pfwp (WordPerfect)
	file.push_back(0x0A);   // file type pfwpdoc (WordPerfect document)
	file.push_back(0x04);   // major version 4
	file.push_back(0x00);   // minor version 0
	put16(file, 0x0000);    // encryption key: none
	put16(file, 0x0000);    // reserved
	for (size_t i = 0; i < m_body.size(); i++)
		file.push_back(m_body[i]);

	FILE *f = std::fopen(m_outputFileName.c_str(), "wb");
	if (!f)
	{
		std::fprintf(stderr, "WP4Generator: cannot open '%s' for writing\n",
		             m_outputFileName.c_str());
		return;
	}
	if (!file.empty())
		std::fwrite(&file[0], 1, file.size(), f);
	std::fclose(f);
}

// ---- framing / gates -------------------------------------------------------

void WP4Generator::emitVariableGroup(unsigned char groupID, unsigned char subGroup,
                                     const std::vector<unsigned char> &payload)
{
	unsigned length = (unsigned)payload.size() + 4;
	std::vector<unsigned char> &out = *m_out;
	out.push_back(groupID);
	out.push_back(subGroup);
	put16(out, length);
	for (size_t i = 0; i < payload.size(); i++)
		out.push_back(payload[i]);
	put16(out, length);
	out.push_back(subGroup);
	out.push_back(groupID);
}

// emit a complete prebuilt variable-length function with explicit framing
static void pushFramed(std::vector<unsigned char> &out, unsigned char groupID,
                       unsigned char subGroup, const std::vector<unsigned char> &payload)
{
	unsigned length = (unsigned)payload.size() + 4;
	out.push_back(groupID);
	out.push_back(subGroup);
	out.push_back((unsigned char)(length & 0xff));
	out.push_back((unsigned char)((length >> 8) & 0xff));
	for (size_t i = 0; i < payload.size(); i++)
		out.push_back(payload[i]);
	out.push_back((unsigned char)(length & 0xff));
	out.push_back((unsigned char)((length >> 8) & 0xff));
	out.push_back(subGroup);
	out.push_back(groupID);
}

void WP4Generator::flushParGate()
{
	if (m_parGateOpen)
	{
		m_out->push_back(W4_PAR_GATE_OFF);
		m_parGateOpen = false;
	}
}

void WP4Generator::emitGated(const std::vector<unsigned char> &func)
{
	// Paragraph-oriented functions must appear inside $AA/$AB paragraph gates
	// (manual, "Function Orientation": every function except the
	// character-oriented ones). $AA must follow a hard end-of-line, temp
	// end-of-line, or start-of-file. To keep that invariant we keep ONE gate
	// open at a time: later gated functions flow into the open gate instead
	// of starting a fresh pair (a fresh $AA directly after an $AB would have
	// no end-of-line before it and would be deleted by the formatter), and
	// the first non-gated emission closes it via flushParGate().
	if (!m_parGateOpen)
	{
		m_out->push_back(W4_PAR_GATE_ON);
		m_parGateOpen = true;
	}
	for (size_t i = 0; i < func.size(); i++)
		m_out->push_back(func[i]);
}

void WP4Generator::emitEOL(unsigned char eolType)
{
	// Hard return/page use the condensed single-byte forms ($80/$81) that
	// WordPerfect 4.0 itself writes (Appendix A, step 7). Table cell/row
	// breaks keep the manual-sanctioned short form:
	//   <$DC><type>[$0002]<type><$DC>
	flushParGate();
	std::vector<unsigned char> &out = *m_out;
	if (eolType == W4_EOL_HARD_RETURN)
	{
		out.push_back(0x80); // Condensed Hard Return -> $DC/$02
		return;
	}
	if (eolType == W4_EOL_HARD_PAGE)
	{
		out.push_back(0x81); // Condensed Hard Page -> $DC/$07
		return;
	}
	out.push_back(W4_EOL_GROUP);
	out.push_back(eolType);
	put16(out, 0x0002);
	out.push_back(eolType);
	out.push_back(W4_EOL_GROUP);
}

// ---- attributes ------------------------------------------------------------

void WP4Generator::attributeOn(unsigned char attr)
{
	// $C3 = Attribute ON/OFF: <$C3><attr><state><$C3>, state msb (bit 7) =
	// old value, lsb (bit 0) = new value. Turning ON: old=0,new=1 -> $01.
	flushParGate();
	std::vector<unsigned char> &out = *m_out;
	out.push_back(W4_ATTRIBUTE);
	out.push_back(attr);
	out.push_back(0x01);
	out.push_back(W4_ATTRIBUTE);
}

void WP4Generator::attributeOff(unsigned char attr)
{
	// Turning OFF: old=1,new=0 -> $80.
	flushParGate();
	std::vector<unsigned char> &out = *m_out;
	out.push_back(W4_ATTRIBUTE);
	out.push_back(attr);
	out.push_back(0x80);
	out.push_back(W4_ATTRIBUTE);
}

std::vector<unsigned char> WP4Generator::attrsFromProps(
        const librevenge::RVNGPropertyList &propList)
{
	std::vector<unsigned char> attrs;
	if (propList["fo:font-weight"] && propList["fo:font-weight"]->getStr() == "bold")
		attrs.push_back(W4_ATTR_BOLD);
	if (propList["fo:font-style"] && propList["fo:font-style"]->getStr() == "italic")
		attrs.push_back(W4_ATTR_ITALICS);
	if (propList["style:text-underline-type"])
	{
		if (propList["style:text-underline-type"]->getStr() == "double")
			attrs.push_back(W4_ATTR_DOUBLE_UNDERLINE);
		else
			attrs.push_back(W4_ATTR_UNDERLINE);
	}
	if (propList["style:text-position"])
	{
		const std::string pos = propList["style:text-position"]->getStr().cstr();
		if (pos == "super") attrs.push_back(W4_ATTR_SUPERSCRIPT);
		else if (pos == "sub") attrs.push_back(W4_ATTR_SUBSCRIPT);
	}
	if (propList["style:text-line-through-type"])
		attrs.push_back(W4_ATTR_STRIKE_OUT);
	return attrs;
}

unsigned WP4Generator::parseHexColor(const std::string &s, unsigned fallback)
{
	const char *p = s.c_str();
	if (*p == '#') p++;
	if (std::strlen(p) < 6) return fallback;
	unsigned v = 0;
	for (int i = 0; i < 6; i++)
	{
		char c = p[i];
		unsigned d;
		if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
		else return fallback;
		v = (v << 4) | d;
	}
	return v;
}

// ---- fonts / sizes / colors ------------------------------------------------

WP4Generator::MacFont WP4Generator::macFontFor(const std::string &family)
{
	std::string canonical;
	int cls = fonttypeface::classify(family, canonical);
	if (cls >= 0)
	{
		static const unsigned char ids[4] = { 22, 21, 34, 20 }; // Courier/Helvetica/Palatino/Times
		static const char *const names[4] = { "Courier", "Helvetica", "Palatino", "Times" };
		return MacFont{ids[cls], names[cls]};
	}
	// Unclassifiable: keep the ODT name verbatim (WordPerfect for Mac
	// selects by name, so an installed custom font still resolves) with
	// Geneva's ID as a well-formed placeholder.
	return MacFont{3, family};
}

void WP4Generator::emitFontChange(const MacFont &oldFont, const MacFont &target)
{
	// $D1/$01 = Set Text Font (Length Variable):
	//   {stamp}[old font #][old type][new font #][new type]<Pascal name>
	// Font type 1 = positive = regular font (negative would mean symbol).
	// {Document stamp} 0 forces WP to take the font from the Pascal name.
	flushParGate();
	std::vector<unsigned char> p;
	put32(p, 0);                                     // {document stamp}
	put16(p, oldFont.id);                            // [old font #]
	put16(p, 1);                                     // [old font type]
	put16(p, target.id);                             // [new font #]
	put16(p, 1);                                     // [new font type]
	p.push_back((unsigned char)target.name.size());  // Pascal string
	for (size_t i = 0; i < target.name.size(); i++)
		p.push_back((unsigned char)target.name[i]);
	emitVariableGroup(W4_FONT_GROUP, W4_SUB_TEXT_FONT, p);
}

void WP4Generator::emitFontSizeChange(int oldPoints, int targetPoints)
{
	// $D1/$02 = Set Font Size (Length 8): [old size][new size], integer points.
	flushParGate();
	std::vector<unsigned char> p;
	put16(p, (unsigned)oldPoints);  // [old font size]
	put16(p, (unsigned)targetPoints); // [new font size]
	emitVariableGroup(W4_FONT_GROUP, W4_SUB_FONT_SIZE, p);
}

void WP4Generator::emitColorChange(unsigned oldRGB, unsigned rgb)
{
	// $D1/$00 = Set Text Color (Length 16): [old R][old G][old B] followed by
	// [new R][new G][new B], each a U16 in 0..65535. 8-bit -> 16-bit: v * 257.
	flushParGate();
	std::vector<unsigned char> p;
	for (int i = 0; i < 3; i++)
		put16(p, ((oldRGB >> (16 - 8 * i)) & 0xff) * 257);
	for (int i = 0; i < 3; i++)
		put16(p, ((rgb >> (16 - 8 * i)) & 0xff) * 257);
	emitVariableGroup(W4_FONT_GROUP, W4_SUB_TEXT_COLOR, p);
}

// ---- page / paragraph formatting -------------------------------------------

unsigned char WP4Generator::justificationFromProps(const librevenge::RVNGPropertyList &propList)
{
	if (!propList["fo:text-align"])
		return 0xFF; // "don't change" sentinel
	std::string align = propList["fo:text-align"]->getStr().cstr();
	// WP4 $D0/$06 modes: 0=Left, 1=Center, 2=Right, 3=Full -- NOT the WP5
	// order (0=left, 1=justify, 2=center, 3=right)!
	if (align == "center") return 1;
	if (align == "right")  return 2;
	if (align == "justify") return 3;
	return 0; // "left", "start", everything else
}

unsigned long WP4Generator::lineSpacingToPX(const std::string &lineHeight)
{
	// $D0/$02 {spacing}: distance from line to line = {spacing} * linesize,
	// so the stored value is the ratio as PX fixed point ($00010000 = 1.0).
	std::string s = lineHeight;
	if (s.empty() || s == "normal")
		return 0x00010000;
	if (s.find('%') == std::string::npos)
		return 0; // absolute-length spacing not translated
	double ratio = std::atof(s.c_str()) / 100.0;
	if (ratio <= 0.0)
		return 0;
	unsigned long v = (unsigned long)(ratio * 65536.0 + 0.5);
	if (v == 0) v = 1;
	return v;
}

void WP4Generator::openPageSpan(const librevenge::RVNGPropertyList &propList)
{
	// Margin TARGETS are recorded here; because margin functions are
	// paragraph-oriented they must sit inside $AA/$AB gates, so they are
	// actually emitted at the next main-body openParagraph.
	if (propList["fo:margin-left"] && m_marginLeftT != propList["fo:margin-left"]->getDouble())
		m_marginDirty = true;
	if (propList["fo:margin-right"] && m_marginRightT != propList["fo:margin-right"]->getDouble())
		m_marginDirty = true;
	if (propList["fo:margin-top"] && m_marginTopT != propList["fo:margin-top"]->getDouble())
		m_marginDirty = true;
	if (propList["fo:margin-bottom"] && m_marginBottomT != propList["fo:margin-bottom"]->getDouble())
		m_marginDirty = true;
	if (propList["fo:margin-left"])
		m_marginLeftT = propList["fo:margin-left"]->getDouble();
	if (propList["fo:margin-right"])
		m_marginRightT = propList["fo:margin-right"]->getDouble();
	if (propList["fo:margin-top"])
		m_marginTopT = propList["fo:margin-top"]->getDouble();
	if (propList["fo:margin-bottom"])
		m_marginBottomT = propList["fo:margin-bottom"]->getDouble();
}

void WP4Generator::emitIndent(unsigned char def, double adjInches)
{
	// $C2 = Indent (Length 7): <$C2><def>{margin adjustment}<$C2>; def
	// 0 = indent (left), 1 = left/right indent. {Margin adjustment} is a PX
	// fixed-point delta applied to the current margin(s) for this paragraph;
	// WP reverts it at the next hard end-of-line.
	flushParGate();
	std::vector<unsigned char> &out = *m_out;
	out.push_back(W4_INDENT_GROUP);
	out.push_back(def);
	put32(out, px(adjInches));
	out.push_back(W4_INDENT_GROUP);
}

void WP4Generator::openParagraph(const librevenge::RVNGPropertyList &propList)
{
	// --- 1. paragraph-oriented formatting functions, inside $AA/$AB gates ---
	// (A gate opened by a preceding openListElement may already be open; the
	// emitGated machinery merges everything into one valid pair.)
	std::vector<unsigned char> f;
	if (m_out == &m_body && m_marginDirty)
	{
		// margins are body-document state, not header/footer/note state
		// $D0/$01 Set Horizontal Margins (Length 20): PX fixed-point
		// distances from the PAPER edge; old = applied values.
		std::vector<unsigned char> p;
		put32(p, px(m_marginLeft));
		put32(p, px(m_marginRight));
		put32(p, px(m_marginLeftT));
		put32(p, px(m_marginRightT));
		pushFramed(f, W4_PAGE_FORMAT, W4_SUB_HORZ_MARGINS, p);
		emitGated(f);

		// $D0/$05 Set Vertical Margins (Length 20)
		f.clear();
		p.clear();
		put32(p, px(m_marginTop));
		put32(p, px(m_marginBottom));
		put32(p, px(m_marginTopT));
		put32(p, px(m_marginBottomT));
		pushFramed(f, W4_PAGE_FORMAT, W4_SUB_VERT_MARGINS, p);
		emitGated(f);

		m_marginLeft = m_marginLeftT;
		m_marginRight = m_marginRightT;
		m_marginTop = m_marginTopT;
		m_marginBottom = m_marginBottomT;
		m_marginDirty = false;
	}

	// Justification and spacing also apply inside header/footer and note
	// sub-documents. In particular, a page-number footer's alignment decides
	// which side the page number is placed on (see endHeaderFooter).
	// $D0/$06 Set Justification Mode (Length 6): <old mode><new mode>
	unsigned char just = justificationFromProps(propList);
	if (just != 0xFF && just != m_currentJustification)
	{
		f.clear();
		std::vector<unsigned char> p;
		p.push_back(m_currentJustification); // old
		p.push_back(just);                   // new
		pushFramed(f, W4_PAGE_FORMAT, W4_SUB_JUSTIFICATION, p);
		emitGated(f);
		m_currentJustification = just;
	}

	// $D0/$02 Set Line Spacing (Length 12): {old spacing}{new spacing}
	if (propList["fo:line-height"])
	{
		unsigned long spacing = lineSpacingToPX(propList["fo:line-height"]->getStr().cstr());
		if (spacing != 0 && spacing != m_currentSpacing)
		{
			f.clear();
			std::vector<unsigned char> p;
			put32(p, m_currentSpacing); // old
			put32(p, spacing);          // new
			pushFramed(f, W4_PAGE_FORMAT, W4_SUB_LINE_SPACING, p);
			emitGated(f);
			m_currentSpacing = spacing;
		}
	}

	// --- 2. close the gate: nothing below is paragraph-oriented -------------
	flushParGate();

	// A note's first paragraph: the source's hanging indent (margin-left +
	// negative text-indent) fights the note number introduced by beginNote,
	// so suppress indent/first-line handling here and swallow the note
	// body's own leading tab (WP5 parity).
	if (m_noteFirstParaPending)
	{
		m_noteFirstParaPending = false;
		m_skipNextTab = true;
		return;
	}

	// hard page break before this paragraph (WP5 parity: after paragraph
	// formatting, before indenting)
	if (propList["fo:break-before"] &&
	        propList["fo:break-before"]->getStr() == "page")
		emitEOL(W4_EOL_HARD_PAGE);

	// deferred $C2 indent from the list element open (character-level move)
	if (m_pendingIndentInches > 0.005)
	{
		emitIndent(0x00, m_pendingIndentInches);
		m_pendingIndentInches = 0.0;
	}

	// deferred bullet (unordered list) or space after an auto number
	if (m_pendingBullet)
	{
		m_pendingBullet = false;
		appendCodePoint(0x2022);
		m_out->push_back(0x20);
	}
	else if (m_pendingNumberSpace)
	{
		m_pendingNumberSpace = false;
		m_out->push_back(0x20);
	}

	// paragraph indentation: left indent ($C2/$00), or a block indent
	// ($C2/$01) when both margins are indented (adjustment applies to both).
	double indentLeft  = propList["fo:margin-left"]  ? propList["fo:margin-left"]->getDouble()  : 0.0;
	double indentRight = propList["fo:margin-right"] ? propList["fo:margin-right"]->getDouble() : 0.0;
	if (indentLeft >= 0.05)
	{
		unsigned char def = (indentRight >= 0.05) ? 0x01 : 0x00;
		double adj = (indentRight >= 0.05 && indentRight > indentLeft)
		             ? indentRight : indentLeft;
		emitIndent(def, adj);
	}
	// first-line indent => leading tabs on the first line only (WP5 parity)
	double firstLine = propList["fo:text-indent"] ? propList["fo:text-indent"]->getDouble() : 0.0;
	int nTabs = (int)(firstLine / 0.5 + 0.5);
	for (int i = 0; i < nTabs; i++)
		insertTab();

	// A hard return (emitted at closeParagraph) separates paragraphs.
}

void WP4Generator::closeParagraph()
{
	emitEOL(W4_EOL_HARD_RETURN);
}

// ---- text ------------------------------------------------------------------

void WP4Generator::appendCodePoint(unsigned long cp)
{
	if (cp >= 0x20 && cp <= 0x7E)
	{
		m_out->push_back((unsigned char)cp); // plain ASCII
		return;
	}
	if (cp == 0xA0)
	{
		// no-break space -> single-byte Hard Space ("Non-wrap space")
		flushParGate();
		m_out->push_back(W4_HARD_SPACE);
		return;
	}

	// Extended character $C0 (length 5). The file stores the DISPLAYED Mac
	// character followed by the defining WP character. Writing a WP 5.1
	// character puts $00 in the Mac position:
	//   <$C0> <$00> <WP character set # from WP 5.1> <Character> <$C0>
	unsigned lo = 0, hi = s_wp5CharMapSize;
	while (lo < hi)
	{
		unsigned mid = lo + (hi - lo) / 2;
		if (s_wp5CharMap[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < s_wp5CharMapSize && s_wp5CharMap[lo].cp == cp)
	{
		m_out->push_back(W4_EXT_CHAR);
		m_out->push_back(0x00); // Mac character: $00 = derive from WP char
		m_out->push_back(s_wp5CharMap[lo].charSet);
		m_out->push_back(s_wp5CharMap[lo].charIndex);
		m_out->push_back(W4_EXT_CHAR);
		return;
	}
	m_out->push_back('?'); // unmapped: visible placeholder
}

void WP4Generator::insertText(const librevenge::RVNGString &text)
{
	flushParGate(); // text is never paragraph-oriented
	const char *s = text.cstr();
	if (!s)
		return;
	const unsigned char *p = (const unsigned char *)s;
	while (*p)
	{
		unsigned long cp = 0;
		int extra = 0;
		if (*p < 0x80)
		{
			cp = *p;
		}
		else if ((*p & 0xE0) == 0xC0)
		{
			cp = *p & 0x1F;
			extra = 1;
		}
		else if ((*p & 0xF0) == 0xE0)
		{
			cp = *p & 0x0F;
			extra = 2;
		}
		else if ((*p & 0xF8) == 0xF0)
		{
			cp = *p & 0x07;
			extra = 3;
		}
		else
		{
			cp = '?';
		}
		p++;
		for (int i = 0; i < extra; i++)
		{
			if ((*p & 0xC0) != 0x80)
			{
				cp = '?';
				break;
			}
			cp = (cp << 6) | (*p & 0x3F);
			p++;
		}
		appendCodePoint(cp);
	}
}

void WP4Generator::insertSpace()
{
	flushParGate();
	m_out->push_back(0x20);
}

void WP4Generator::insertTab()
{
	// Drop the note body's leading tab (armed on the note's first paragraph)
	// so the text follows the note number directly instead of jumping to a
	// tab stop (WP5 parity).
	if (m_skipNextTab)
	{
		m_skipNextTab = false;
		return;
	}
	flushParGate();
	// $C1 = HorzMove_ (Length 8): <$C1><def><mode/type>{horizontal
	// adjustment}<$C1>; def 0 = plain tab, mode/type 0 = no leader + normal.
	std::vector<unsigned char> &out = *m_out;
	out.push_back(W4_TAB_GROUP);
	out.push_back(0x00); // def: tab
	out.push_back(0x00); // mode/type: no leader, normal tab
	put32(out, 0);       // {horizontal adjustment} = 0
	out.push_back(W4_TAB_GROUP);
}

void WP4Generator::insertLineBreak()
{
	emitEOL(W4_EOL_HARD_RETURN);
}

// ---- spans (font/size/color + character attributes) ------------------------

void WP4Generator::openSpan(const librevenge::RVNGPropertyList &propList)
{
	// --- font/size (character-oriented $D1 groups, emitted when changed) ---
	MacFont target = m_currentFont;
	if (propList["style:font-name"])
		target = macFontFor(propList["style:font-name"]->getStr().cstr());

	int targetSize = m_currentFontSize;
	if (propList["fo:font-size"])
	{
		double pts = propList["fo:font-size"]->getDouble();
		if (pts < 1) pts = 1;
		if (pts > 255) pts = 255; // [font size] is an integer point count
		targetSize = (int)(pts + 0.5);
	}

	if (!(target == m_currentFont))
	{
		emitFontChange(m_currentFont, target);
		m_currentFont = target;
	}
	if (targetSize != m_currentFontSize)
	{
		emitFontSizeChange(m_currentFontSize, targetSize);
		m_currentFontSize = targetSize;
	}

	// --- text color ----------------------------------------------------------
	unsigned targetColor = m_currentColor;
	if (propList["fo:color"])
		targetColor = parseHexColor(propList["fo:color"]->getStr().cstr(), m_currentColor);

	// --- remember per-span state, then apply attributes --------------------
	m_fontStack.push_back(m_currentFont);
	m_fontSizeStack.push_back(m_currentFontSize);
	m_colorStack.push_back(m_currentColor);
	if (targetColor != m_currentColor)
	{
		emitColorChange(m_currentColor, targetColor);
		m_currentColor = targetColor;
	}

	// Attributes for THIS span. Spans nest (a paragraph-style wrapper span
	// may contain inner content spans), so push each span's set onto a stack
	// and turn them off when the matching span closes.
	std::vector<unsigned char> attrs = attrsFromProps(propList);
	for (size_t i = 0; i < attrs.size(); i++)
		attributeOn(attrs[i]);
	m_attributeStack.push_back(attrs);
}

void WP4Generator::closeSpan()
{
	// turn off this span's attributes in reverse order
	if (!m_attributeStack.empty())
	{
		const std::vector<unsigned char> &attrs = m_attributeStack.back();
		for (size_t i = attrs.size(); i > 0; i--)
			attributeOff(attrs[i - 1]);
		m_attributeStack.pop_back();
	}

	// restore the font/size that were active before this span
	if (!m_fontStack.empty())
	{
		MacFont previousFont = m_fontStack.back();
		int previousSize = m_fontSizeStack.back();
		m_fontStack.pop_back();
		m_fontSizeStack.pop_back();
		if (!(m_currentFont == previousFont))
		{
			emitFontChange(m_currentFont, previousFont); // old -> restored
			m_currentFont = previousFont;
		}
		if (m_currentFontSize != previousSize)
		{
			emitFontSizeChange(m_currentFontSize, previousSize);
			m_currentFontSize = previousSize;
		}
	}

	// restore the text color that was active before this span
	if (!m_colorStack.empty())
	{
		unsigned previous = m_colorStack.back();
		m_colorStack.pop_back();
		if (m_currentColor != previous)
		{
			emitColorChange(m_currentColor, previous);
			m_currentColor = previous;
		}
	}
}

// ---- header/footer sub-documents (WP4 group $D5) ---------------------------

void WP4Generator::beginHeaderFooter(unsigned char subGroup,
                                     const librevenge::RVNGPropertyList & /*propList*/)
{
	// Remember whether the body already has a paragraph gate open (typically
	// from a preceding header/footer function), close it logically without
	// writing $AB -- writing it here and reopening with $AA at endHeaderFooter
	// would produce an $AA directly after $AB, which the formatter deletes.
	m_savedParGateOpen = m_parGateOpen;
	m_parGateOpen = false;
	m_hfSubGroup = subGroup;
	m_hfHasPageNumber = false;

	// save main-document state; the sub-document starts with WP defaults
	m_savedFont = m_currentFont;
	m_savedFontSize = m_currentFontSize;
	m_savedColor = m_currentColor;
	m_savedJustification = m_currentJustification;
	m_savedSpacing = m_currentSpacing;
	m_savedAttributeStack = m_attributeStack;
	m_currentFont = MacFont{3, "Geneva"};
	m_currentFontSize = 12;
	m_currentColor = 0;
	m_currentJustification = 0;
	m_currentSpacing = 0x00010000;
	m_attributeStack.clear();
	m_subDoc.clear();
	m_out = &m_subDoc;
}

void WP4Generator::endHeaderFooter()
{
	// capture the sub-document bytes and its page-number status first
	std::vector<unsigned char> sub = m_subDoc;
	bool hadPageNumber = m_hfHasPageNumber;
	unsigned char hfJustification = m_currentJustification; // alignment INSIDE the H/F

	m_out = &m_body; // the $D5/$D0 functions below go into the main body
	m_currentFont = m_savedFont;
	m_currentFontSize = m_savedFontSize;
	m_currentColor = m_savedColor;
	m_currentJustification = m_savedJustification;
	m_currentSpacing = m_savedSpacing;
	m_attributeStack = m_savedAttributeStack;
	// Re-adopt the body's gate state so this function joins an already-open
	// paragraph gate (consecutive header/footer definitions) instead of
	// opening a new one right after $AB.
	m_parGateOpen = m_savedParGateOpen;

	if (hadPageNumber)
	{
		// A header/footer whose only purpose is the page number becomes a
		// $D0/$08 Set Page Number Position function instead (WP5 parity).
		// Positions: 0 none; 1-4 top left/center/right/left&right;
		// 5-8 same, bottom. Column from the alignment used in the H/F.
		bool isFooter = (m_hfSubGroup == W4_SUB_FOOTER_A);
		unsigned char col = 1; // left
		if (hfJustification == 1) col = 2;      // center
		else if (hfJustification == 2) col = 3; // right
		unsigned char posCode = (unsigned char)((isFooter ? 4 : 0) + col);
		if (posCode != m_pageNumPos)
		{
			std::vector<unsigned char> p;
			p.push_back(m_pageNumPos); // old
			p.push_back(posCode);      // new
			std::vector<unsigned char> f;
			pushFramed(f, W4_PAGE_FORMAT, W4_SUB_PAGE_NUM_POS, p);
			emitGated(f);
			m_pageNumPos = posCode;
		}
		return;
	}

	// $D5/<0-3> Header/Footer (Length Variable), Page-oriented -> gated:
	// <flags>{stamp}{ptr}<old def>{old height}[old len]<new def>{new height}
	// [new len]<text>. Occurrence def bits: bit 0 = even, bit 1 = odd
	// (all pages = $03). Formatter-supplied values are zero in new files.
	unsigned char occurrence = 0x03;
	if (m_hfOccurrenceStr == "even") occurrence = 0x01;
	else if (m_hfOccurrenceStr == "odd") occurrence = 0x02;

	std::vector<unsigned char> f;
	f.push_back(W4_HEADER_FOOTER_GROUP);
	f.push_back(m_hfSubGroup);
	unsigned length = (unsigned)(23 + sub.size() + 4);
	put16(f, length);
	// old block (no previous header/footer of this slot)
	f.push_back(0x00);        // <format flags>
	put32(f, 0);              // {document stamp}
	put32(f, 0);              // {ptr to header/footer window}
	f.push_back(0x00);        // <old def>: none
	put32(f, 0);              // {old height}
	put16(f, 0);              // [old header length] = 0 -> no old text
	// new block
	f.push_back(occurrence);  // <new def>
	put32(f, 0);              // {new height}
	put16(f, (unsigned)sub.size()); // [new header length]
	for (size_t i = 0; i < sub.size(); i++)
		f.push_back(sub[i]);
	put16(f, length);
	f.push_back(m_hfSubGroup);
	f.push_back(W4_HEADER_FOOTER_GROUP);
	emitGated(f);
	// Leave the gate open: a following footer/header definition joins it, and
	// the first text/attribute/hard-return in the body closes it. Flushing
	// here would put the next $AA directly after $AB (deleted by WP).
}

void WP4Generator::openHeader(const librevenge::RVNGPropertyList &propList)
{
	if (propList["librevenge:occurrence"])
		m_hfOccurrenceStr = propList["librevenge:occurrence"]->getStr().cstr();
	else
		m_hfOccurrenceStr = "all";
	beginHeaderFooter(W4_SUB_HEADER_A, propList);
}

void WP4Generator::closeHeader()
{
	endHeaderFooter();
}

void WP4Generator::openFooter(const librevenge::RVNGPropertyList &propList)
{
	if (propList["librevenge:occurrence"])
		m_hfOccurrenceStr = propList["librevenge:occurrence"]->getStr().cstr();
	else
		m_hfOccurrenceStr = "all";
	beginHeaderFooter(W4_SUB_FOOTER_A, propList);
}

void WP4Generator::closeFooter()
{
	endHeaderFooter();
}

// ---- fields ----------------------------------------------------------------

void WP4Generator::insertField(const librevenge::RVNGPropertyList &propList)
{
	const librevenge::RVNGProperty *type = propList["librevenge:field-type"];
	if (!type)
		return;
	std::string ft = type->getStr().cstr();

	if (ft == "text:page-number")
	{
		// Inside a header/footer, a page-number field marks this as a
		// page-number header/footer (handled in endHeaderFooter as a $D0/$08).
		if (m_out == &m_subDoc)
			m_hfHasPageNumber = true;
		return;
	}
	// other fields have no WP4 equivalent
}

// ---- lists / automatic paragraph numbering ($D2/$02 + $D8/$01) -------------

std::string toUpperRoman(int n)
{
	static const int v[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
	static const char *r[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
	std::string s;
	if (n < 1) n = 1;
	for (int i = 0; i < 13; i++)
		while (n >= v[i]) { s += r[i]; n -= v[i]; }
	for (size_t i = 0; i < s.size(); i++)
		s[i] = (char)std::toupper((unsigned char)s[i]);
	return s;
}

// ODT num-format char + suffix -> the $D2/$02 def byte $ps (high nibble =
// punctuation 0 none, 1 period, 2 close-paren, 3 parenthesized; low nibble =
// style 0 upperRoman, 1 lowerRoman, 2 upperLetter, 3 lowerLetter, 4 arabic,
// 5 legal). 
unsigned char parNumDefByte(char fmt, const std::string &suffix)
{
	unsigned char punct = 0;
	if (suffix.size() >= 2 && suffix[0] == '(' && suffix[1] == ')')
		punct = 3;
	else if (!suffix.empty())
	{
		if (suffix[0] == '.') punct = 1;
		else if (suffix[0] == ')') punct = 2;
	}
	unsigned char style;
	switch (fmt)
	{
	case 'i': style = 1; break;
	case 'A': style = 2; break;
	case 'a': style = 3; break;
	case 'I': style = 0; break;
	default:  style = 4; break; // '1' arabic
	}
	return (unsigned char)((punct << 4) | style);
}

void WP4Generator::emitParagraphNumberDefinition(const char levelFormat[8],
                                                 const std::string levelSuffix[8])
{
	// $D2/$02 = Define Paragraph Numbering (Length 52): <old defs 1-8>,
	// [old levels 1-8], <new defs 1-8>, [new levels 1-8]. Old defs use the
	// WP default style (arabic with a period = $14); levels are
	// formatter-supplied counters -> zero.
	std::vector<unsigned char> p;
	std::vector<unsigned char> defs(8);
	for (int i = 0; i < 8; i++)
		defs[i] = parNumDefByte(levelFormat[i], levelSuffix[i]);
	for (int i = 0; i < 8; i++) p.push_back(0x14);   // old defs: WP default
	for (int i = 0; i < 8; i++) put16(p, 0);         // old levels
	for (int i = 0; i < 8; i++) p.push_back(defs[i]); // new defs
	for (int i = 0; i < 8; i++) put16(p, 0);         // new levels
	// paragraph-oriented function -> goes inside the paragraph gates
	std::vector<unsigned char> f;
	pushFramed(f, W4_DEF_GROUP, W4_SUB_PAR_NUM_DEF, p);
	emitGated(f); // gate stays open for the following $D8/$01 + formatting
}

void WP4Generator::openOrderedListLevel(const librevenge::RVNGPropertyList &propList)
{
	char fmt = '1'; // arabic default
	if (propList["style:num-format"])
	{
		std::string f = propList["style:num-format"]->getStr().cstr();
		if (!f.empty()) fmt = f[0];
	}
	std::string suffix = propList["style:num-suffix"] ? propList["style:num-suffix"]->getStr().cstr() : ".";
	std::string prefix = propList["style:num-prefix"] ? propList["style:num-prefix"]->getStr().cstr() : "";

	// The numbering definition covers all 8 levels at once. ODT streams
	// levels lazily, but the reader hands us every level's style up front
	// via "librevenge:level-definitions" (see ODTReader::handleList).
	if (!m_outlineOn)
	{
		char levelFormat[8];
		std::string levelSuffix[8];
		for (int i = 0; i < 8; i++)
		{
			levelFormat[i] = fmt;
			levelSuffix[i] = suffix.empty() ? "." : suffix;
		}
		const librevenge::RVNGPropertyListVector *defs = propList.child("librevenge:level-definitions");
		if (defs)
		{
			for (unsigned long i = 0; i < defs->count() && i < 8; i++)
			{
				const librevenge::RVNGPropertyList &e = (*defs)[i];
				if (e["style:num-format"])
				{
					std::string f = e["style:num-format"]->getStr().cstr();
					if (!f.empty()) levelFormat[i] = f[0];
				}
				if (e["style:num-suffix"])
				{
					std::string s = e["style:num-suffix"]->getStr().cstr();
					levelSuffix[i] = s.empty() ? "." : s;
				}
			}
		}
		emitParagraphNumberDefinition(levelFormat, levelSuffix);
		m_outlineOn = true;
	}

	if (m_listLevel < 7) m_listLevel++;
	int L = m_listLevel;
	m_listOrdered[L] = true;
	m_listFormat[L] = fmt;
	m_listSuffix[L] = suffix;
	m_listPrefix[L] = prefix;
	int start = propList["text:start-value"] ? propList["text:start-value"]->getInt() : 1;
	m_listCounter[L] = start - 1;
}

void WP4Generator::openUnorderedListLevel(const librevenge::RVNGPropertyList & /*propList*/)
{
	if (m_listLevel < 7) m_listLevel++;
	m_listOrdered[m_listLevel] = false;
}

void WP4Generator::closeOrderedListLevel()
{
	if (m_listLevel >= 0) m_listLevel--;
}

void WP4Generator::closeUnorderedListLevel()
{
	if (m_listLevel >= 0) m_listLevel--;
}

void WP4Generator::openListElement(const librevenge::RVNGPropertyList & /*propList*/)
{
	if (m_listLevel < 0)
		return;
	int L = m_listLevel;

	// Depth indentation is deferred to the paragraph open as a $C2 indent:
	// $C1 tabs would have to precede the $D8/$01 gate (invalid) and can't
	// appear inside it (they move the pen, they don't set paragraph state).
	m_pendingIndentInches = 0.5 * (L + 1);

	if (m_listOrdered[L])
	{
		m_listCounter[L]++;
		// $D8/$01 = Insert Paragraph Number (Length 21): <new level number>
		// followed by the 8 level-counter words (merged into the open
		// paragraph gate together with the next openParagraph's functions).
		std::vector<unsigned char> p;
		p.push_back((unsigned char)(L & 0x7F)); // <new level number>
		for (int i = 0; i < 8; i++)
			put16(p, (i <= L && m_listCounter[i] > 0) ? (unsigned)m_listCounter[i] : 0);
		std::vector<unsigned char> f;
		pushFramed(f, W4_DISPLAY_GROUP, W4_SUB_INSERT_PAR_NUM, p);
		emitGated(f);
		m_pendingNumberSpace = true; // space between the number and the text
	}
	else
	{
		// Unordered level: insert a literal bullet at the paragraph open
		// (after the gates close).
		m_pendingBullet = true;
	}
}

// ---- footnotes / endnotes (inline sub-documents, WP4 group $D6) -------------

void WP4Generator::beginNote(bool endnote, const librevenge::RVNGPropertyList &numberProps)
{
	m_noteIsEndnote = endnote;
	m_noteNumber = endnote ? ++m_endnoteCounter : ++m_footnoteCounter;

	flushParGate(); // the body gate closes before the note machinery

	// save main-document state and redirect emission into the sub-document
	m_savedFont = m_currentFont;
	m_savedFontSize = m_currentFontSize;
	m_savedColor = m_currentColor;
	m_savedJustification = m_currentJustification;
	m_savedSpacing = m_currentSpacing;
	m_savedAttributeStack = m_attributeStack;
	m_currentFont = MacFont{3, "Geneva"};
	m_currentFontSize = 12;
	m_currentColor = 0;
	m_currentJustification = 0;
	m_currentSpacing = 0x00010000;
	m_attributeStack.clear();
	m_subDoc.clear();
	m_out = &m_subDoc;
	m_parGateOpen = false;

	// The note text starts with the single-byte "Footnote/Endnote number"
	// code ($A2 / $A4; the manual converts these to $D8/$07 and $D8/$08 on
	// formatting), so real WordPerfect prints the note's number before its
	// text. The number carries the source's marker style (e.g. superscript),
	// bracketing the code exactly like the other attributes.
	std::vector<unsigned char> numAttrs = attrsFromProps(numberProps);
	for (size_t i = 0; i < numAttrs.size(); i++)
		attributeOn(numAttrs[i]);
	m_out->push_back(endnote ? W4_ENDNOTE_NUM_BYTE : W4_FOOTNOTE_NUM_BYTE);
	for (size_t i = numAttrs.size(); i > 0; i--)
		attributeOff(numAttrs[i - 1]);

	// The next paragraph is the note's first: skip its hanging indent and
	// leading tab so the text follows the number directly.
	m_noteFirstParaPending = true;
}

void WP4Generator::endNote()
{
	// A degenerate note (no paragraph) must not leak these into the body.
	m_noteFirstParaPending = false;
	m_skipNextTab = false;

	// close any gate the note's own paragraphs opened, then capture
	flushParGate();
	std::vector<unsigned char> sub = m_subDoc;

	unsigned noteNumber = m_noteNumber;
	bool wasEndnote = m_noteIsEndnote;
	unsigned prevNumber = wasEndnote ? (unsigned)(m_endnoteCounter - 1)
	                                 : (unsigned)(m_footnoteCounter - 1);

	m_out = &m_body; // the $D3 seeds and $D6 group below go into the body
	m_currentFont = m_savedFont;
	m_currentFontSize = m_savedFontSize;
	m_currentColor = m_savedColor;
	m_currentJustification = m_savedJustification;
	m_currentSpacing = m_savedSpacing;
	m_attributeStack = m_savedAttributeStack;
	m_parGateOpen = false;

	// Seed the note counter to THIS note's number ($D3/$02 or $D3/$03,
	// Length 8: [old][new]). WP's formatter normally advances the counter
	// itself; setting it explicitly both covers documents where it does not
	// and is a no-op (and therefore auto-deleted) when it does. Old = the
	// number before this note (0 seeds the first).
	std::vector<unsigned char> p;
	put16(p, prevNumber);   // old
	put16(p, noteNumber);   // new
	emitVariableGroup(W4_SET_GROUP, wasEndnote ? W4_SUB_SET_ENN_NUM : W4_SUB_SET_FTN_NUM, p);

	// $D6/$00 Footnote / $D6/$01 Endnote (Length Variable). All
	// formatter-supplied fields are zero in newly created notes ("All
	// formatter supplied values should be set to zero when creating a new
	// footnote"); identical 29-byte zero header for the endnote, per spec.
	std::vector<unsigned char> f;
	f.push_back(W4_NOTE_GROUP);
	f.push_back(wasEndnote ? W4_SUB_ENDNOTE : W4_SUB_FOOTNOTE);
	unsigned length = (unsigned)(29 + sub.size() + 4);
	put16(f, length);
	f.push_back(0x00);  // <format flags>
	put32(f, 0);        // {document stamp}
	put32(f, 0);        // {ptr to footnote/endnote window}
	put16(f, 0);        // [window footnote/endnote #]
	put32(f, 0);        // {footnote size on this page} / {zero}
	put16(f, 0);        // [additional footnote pages] / [$0000]
	put32(f, 0);        // {footnote size on last page} / {zero}
	put32(f, 0);        // {footnote/endnote length}
	put16(f, 0);        // [# of footnote pages] / [$0000]
	put16(f, 0);        // [# of break table entries] = 0 -> no entries
	for (size_t i = 0; i < sub.size(); i++)
		f.push_back(sub[i]);
	put16(f, length);
	f.push_back(wasEndnote ? W4_SUB_ENDNOTE : W4_SUB_FOOTNOTE);
	f.push_back(W4_NOTE_GROUP);
	// $D6 is character-oriented: the note stays anchored mid-line. (Had a
	// paragraph gate been open it was closed in flushParGate above.)
	for (size_t i = 0; i < f.size(); i++)
		m_out->push_back(f[i]);
}

void WP4Generator::openFootnote(const librevenge::RVNGPropertyList &propList)
{
	beginNote(false, propList);
}

void WP4Generator::closeFootnote()
{
	endNote();
}

void WP4Generator::openEndnote(const librevenge::RVNGPropertyList &propList)
{
	beginNote(true, propList);
}

void WP4Generator::closeEndnote()
{
	endNote();
}

// ---- tables ($E2/$01 definition + $DC table EOL short forms) ---------------

void WP4Generator::openTable(const librevenge::RVNGPropertyList &propList)
{
	flushParGate();

	// Collect column widths (inches) from librevenge:table-columns.
	std::vector<double> colWidths;
	const librevenge::RVNGPropertyListVector *cols = propList.child("librevenge:table-columns");
	if (cols)
	{
		for (unsigned long i = 0; i < cols->count() && i < 32; i++)
		{
			const librevenge::RVNGPropertyList &c = (*cols)[i];
			double w = c["style:column-width"] ? c["style:column-width"]->getDouble() : 1.0;
			colWidths.push_back(w);
		}
	}
	if (colWidths.empty())
		colWidths.push_back(1.0); // fallback: one 1-inch column
	unsigned n = (unsigned)colWidths.size();

	m_tableNumColumns = n;
	m_tableNumRows = propList["librevenge:num-rows"] ? (unsigned)propList["librevenge:num-rows"]->getInt() : 1;
	if (m_tableNumRows < 1) m_tableNumRows = 1;
	m_tableRowCount = 0;
	m_tableOffDone = false;
	++m_tableIdCounter; // unique per document

	// $E2/$01 = Table Function (Length Variable), Paragraph-oriented ->
	// gated. Layout per the manual:
	//   {Document stamp} 4   [Table ID1] 2      [Table ID2] 2
	//   <outside top/left/bottom/right border #> 4
	//   [outside top/left/bottom/right RGB].3    24   (3 words = 6 bytes/color)
	//   <cell top/left/bottom/right border #>    4    (default cell border)
	//   [cell top/left/bottom/right RGB].3       24
	//   [table background fill RGB color].3      6
	//   <cell fill pattern #> 1     <table mode> 1   (bit 7 = tables ON)
	//   {offset from left edge} 4
	//   {top/left/bottom/right gutter} 4 each    16
	//   [# of rows] 2   <# of header rows> 1   <# of columns> 1
	//   per column: <column mode> 1, <number format #> 1, {column width} 4,
	//               {right offset for decimal align} 4 = 10 bytes
	// Border 2 = single line for both the outside border and default cell
	// borders; colors black (0,0,0).
	std::vector<unsigned char> p;
	put32(p, 0);                       // {document stamp}
	put16(p, 0);                       // [Table ID1] (formatter counter)
	put16(p, m_tableIdCounter);        // [Table ID2] (unique)
	// A color is "[RGB color].3": three 16-bit words, i.e. 6 bytes each
	// (confirmed by $DC/$08's Length 26 for four colors).
	for (int i = 0; i < 4; i++)  p.push_back(0x02); // outside borders: single
	for (int i = 0; i < 4 * 6; i++) p.push_back(0x00); // outside RGB: black
	for (int i = 0; i < 4; i++)  p.push_back(0x02); // default cell borders
	for (int i = 0; i < 4 * 6; i++) p.push_back(0x00); // cell RGB: black
	for (int i = 0; i < 3; i++)  put16(p, 0);   // fill RGB: black
	p.push_back(0x00);                 // <cell fill pattern #>: none
	p.push_back(0x80);                 // <table mode>: tables ON, left margin
	put32(p, 0);                       // {offset from left edge of paper}
	for (int i = 0; i < 4; i++)  put32(p, 0);  // gutters
	put16(p, m_tableNumRows);          // [# of rows]
	p.push_back(0x00);                 // <# of header rows>
	p.push_back((unsigned char)n);     // <# of columns>
	for (unsigned i = 0; i < n; i++)
	{
		p.push_back(0x00);             // <column mode>: default
		p.push_back(0x00);             // <number format #>: default
		put32(p, px(colWidths[i]));    // {column width}
		put32(p, 0);                   // {right offset for decimal align}
	}

	std::vector<unsigned char> f;
	pushFramed(f, W4_TABLE_GROUP, W4_SUB_TABLE_DEF, p);
	emitGated(f);
	// NOTE: the gate stays open -- the first cell's paragraph functions
	// (and any cell spans) flow into the same $AA/$AB pair.
}

void WP4Generator::openTableRow(const librevenge::RVNGPropertyList & /*propList*/)
{
	m_currentColumn = 0;
	m_tableRowCount++;
}

void WP4Generator::openTableCell(const librevenge::RVNGPropertyList &propList)
{
	unsigned colSpan = propList["table:number-columns-spanned"]
	                   ? (unsigned)propList["table:number-columns-spanned"]->getInt() : 1;
	unsigned rowSpan = propList["table:number-rows-spanned"]
	                   ? (unsigned)propList["table:number-rows-spanned"]->getInt() : 1;
	if (colSpan < 1) colSpan = 1;
	if (rowSpan < 1) rowSpan = 1;

	// $E2/$02 = Set Table Cell Span (Length 6), Table-Cell-oriented ->
	// gated: <# horizontally spanned><# vertically spanned>, where zero
	// means no joined cells and one joins two cells.
	std::vector<unsigned char> f;
	if (colSpan > 1 || rowSpan > 1)
	{
		std::vector<unsigned char> p;
		p.push_back((unsigned char)(colSpan - 1));
		p.push_back((unsigned char)(rowSpan - 1));
		pushFramed(f, W4_TABLE_GROUP, W4_SUB_CELL_SPAN, p);
		emitGated(f);
		flushParGate();
	}

	m_currentColumn += colSpan;
	m_inCell = true;
}

void WP4Generator::closeTableCell()
{
	m_inCell = false;
	// A cell marker is itself an end-of-line, so drop the final hard return the
	// paragraph emitted: keeping it would add an empty paragraph at the cell end.
	if (!m_out->empty() && m_out->back() == 0x80)
		m_out->pop_back();
	// Between cells inside one row: a hard end-of-table-cell $DC/$16. The
	// LAST cell of a row is ended by the row-level code at closeTableRow
	// ($DC/$18) or the table-level one for the final row ($DC/$1A).
	if (m_currentColumn < m_tableNumColumns)
		emitEOL(W4_EOL_TABLE_CELL);
}

void WP4Generator::closeTableRow()
{
	// A row/table marker also ends the cell's last paragraph.
	if (!m_out->empty() && m_out->back() == 0x80)
		m_out->pop_back();
	// $DC/$18 = hard end of table row/cell; for the table's last row,
	// $DC/$1A = hard end of table row/end of table (both short forms,
	// Appendix A).
	if (m_tableRowCount >= m_tableNumRows)
	{
		emitEOL(W4_EOL_TABLE_OFF);
		m_tableOffDone = true;
	}
	else
	{
		emitEOL(W4_EOL_TABLE_ROW);
	}
}

void WP4Generator::closeTable()
{
	// Safety: if the declared row count was short of the rows actually
	// streamed, the table still needs its closing $DC/$1A.
	if (!m_tableOffDone)
	{
		emitEOL(W4_EOL_TABLE_OFF);
		m_tableOffDone = true;
	}
	m_inCell = false;
}
