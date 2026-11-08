/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* odt2wp5 / WP4Generator
 *
 * A librevenge::RVNGTextInterface implementation that consumes the neutral
 * document event stream and emits a WordPerfect 4.0 for Macintosh binary
 * document, per the "Macintosh WordPerfect 4.0 File Format Manual"
 * (Corel, 22 Apr 93 revision).
 *
 * The Mac WP 4.0 file is the ancestor of WP5.1 and shares its architecture:
 * a 16-byte file prefix (FF 'WPC', 32-bit little-endian document pointer,
 * product/type/version bytes) followed by a flat stream of gate-pair
 * functions. Differences from WP5.1 that the generator encodes:
 *   - absolute positions are 72dpi fixed-point PIXELS (PX = in*72*65536),
 *     not 1200ths of an inch (WPU);
 *   - attributes are a single $C3 gate pair with old/new state bits
 *     (WP5 uses two separate opcodes);
 *   - an extended character is <$C0><$00><set><char><$C0> (the Mac byte
 *     first, then the WP 5.1 set/character);
 *   - paragraph-oriented functions must be wrapped in $AA/$AB gates;
 *   - hard returns/pages/table ends are $DC/<type> short-form groups;
 *   - fonts are resolved by Macintosh font name and ID, not by a prefix
 *     font packet table.
 *
 * "This product is not manufactured, approved, or supported by Corel
 *  Corporation or Corel Corporation Limited."
 */

#ifndef WP4GENERATOR_H
#define WP4GENERATOR_H

#include <librevenge/librevenge.h>

#include <string>
#include <utility>
#include <vector>

class WP4Generator : public librevenge::RVNGTextInterface
{
public:
	explicit WP4Generator(const char *outputFileName);
	~WP4Generator() override;

	void startDocument(const librevenge::RVNGPropertyList &propList) override;
	void endDocument() override;

	void openParagraph(const librevenge::RVNGPropertyList &propList) override;
	void closeParagraph() override;

	void openSpan(const librevenge::RVNGPropertyList &propList) override;
	void closeSpan() override;

	void insertText(const librevenge::RVNGString &text) override;
	void insertSpace() override;
	void insertTab() override;
	void insertLineBreak() override;
	void insertField(const librevenge::RVNGPropertyList &propList) override;

	// --- no-op events ---
	void setDocumentMetaData(const librevenge::RVNGPropertyList &) override {}
	void defineEmbeddedFont(const librevenge::RVNGPropertyList &) override {}
	void definePageStyle(const librevenge::RVNGPropertyList &) override {}
	void defineParagraphStyle(const librevenge::RVNGPropertyList &) override {}
	void defineCharacterStyle(const librevenge::RVNGPropertyList &) override {}
	void openLink(const librevenge::RVNGPropertyList &) override {}
	void closeLink() override {}
	void defineSectionStyle(const librevenge::RVNGPropertyList &) override {}
	void openSection(const librevenge::RVNGPropertyList &) override {}
	void closeSection() override {}
	void openComment(const librevenge::RVNGPropertyList &) override {}
	void closeComment() override {}
	void openTextBox(const librevenge::RVNGPropertyList &) override {}
	void closeTextBox() override {}
	void insertCoveredTableCell(const librevenge::RVNGPropertyList &) override {}
	void openFrame(const librevenge::RVNGPropertyList &) override {}
	void closeFrame() override {}
	void insertBinaryObject(const librevenge::RVNGPropertyList &) override {}
	void insertEquation(const librevenge::RVNGPropertyList &) override {}
	void openGroup(const librevenge::RVNGPropertyList &) override {}
	void closeGroup() override {}
	void defineGraphicStyle(const librevenge::RVNGPropertyList &) override {}
	void drawRectangle(const librevenge::RVNGPropertyList &) override {}
	void drawEllipse(const librevenge::RVNGPropertyList &) override {}
	void drawPolygon(const librevenge::RVNGPropertyList &) override {}
	void drawPolyline(const librevenge::RVNGPropertyList &) override {}
	void drawPath(const librevenge::RVNGPropertyList &) override {}
	void drawConnector(const librevenge::RVNGPropertyList &) override {}

	// --- implemented events ---
	void openPageSpan(const librevenge::RVNGPropertyList &propList) override;
	void closePageSpan() override {}
	void openHeader(const librevenge::RVNGPropertyList &propList) override;
	void closeHeader() override;
	void openFooter(const librevenge::RVNGPropertyList &propList) override;
	void closeFooter() override;
	void openOrderedListLevel(const librevenge::RVNGPropertyList &propList) override;
	void openUnorderedListLevel(const librevenge::RVNGPropertyList &propList) override;
	void closeOrderedListLevel() override;
	void closeUnorderedListLevel() override;
	void openListElement(const librevenge::RVNGPropertyList &propList) override;
	void closeListElement() override {}
	void openFootnote(const librevenge::RVNGPropertyList &propList) override;
	void closeFootnote() override;
	void openEndnote(const librevenge::RVNGPropertyList &propList) override;
	void closeEndnote() override;
	void openTable(const librevenge::RVNGPropertyList &propList) override;
	void openTableRow(const librevenge::RVNGPropertyList &propList) override;
	void closeTableRow() override;
	void openTableCell(const librevenge::RVNGPropertyList &propList) override;
	void closeTableCell() override;
	void closeTable() override;

private:
	// A Macintosh font: WordPerfect 4.0 (Mac) selects fonts by classic Mac
	// Font Manager name + ID, so an ODT family is canonicalized to the
	// classic Courier/Helvetica/Palatino/Times quartet (or the ODT name kept
	// verbatim with Geneva's ID when it matches nothing classic).
	struct MacFont
	{
		unsigned char id;   // classic Mac font number
		std::string name;   // classic Mac font name
		bool operator==(const MacFont &o) const
		{ return id == o.id && name == o.name; }
	};

	// append a 16/32-bit little-endian word / the PX inch encoder
	static void put16(std::vector<unsigned char> &v, unsigned value);
	static void put32(std::vector<unsigned char> &v, unsigned long value);
	// inches -> 72dpi fixed-point pixel value (PX), the WP4 unit
	static unsigned long px(double inches);

	// emit a variable-length group: <grp><sub>[Len][payload][Len]<sub><grp>
	// (Len = payload size + 4, mirroring the WP5 "raw size" field: the
	// manual's fixed-function Length values all match payload + 4.)
	void emitVariableGroup(unsigned char groupID, unsigned char subGroup,
	                       const std::vector<unsigned char> &payload);

	// paragraph-gate handling: paragraph-oriented functions must be encased in
	// $AA/$AB (manual, "Function Orientation"). One gate is kept open at a
	// time; non-gated emitters call flushParGate() as their first act so the
	// $AB lands before any text/attribute/EOL byte, and adjacent gated
	// functions merge into a single valid pair (a fresh $AA directly after an
	// $AB has no end-of-line before it and would be deleted).
	bool m_parGateOpen;                     // an $AA is emitted without $AB yet
	bool m_savedParGateOpen;                // gate state saved across sub-docs
	void emitGated(const std::vector<unsigned char> &func); // append inside one gate
	void flushParGate();                    // close an open $AA with $AB
	// short-form end-of-line/page group: <$DC><type>[$0002]<type><$DC>
	void emitEOL(unsigned char eolType);

	// $C3 attribute gate pair: <C3><attr><state><C3> (state bit7 = old value,
	// bit0 = new: 0x01 = turning ON, 0x80 = turning OFF)
	void attributeOn(unsigned char attr);
	void attributeOff(unsigned char attr);

	// ODF text-align -> WP4 justification mode (0 left, 1 center, 2 right,
	// 3 full) and fo:line-height -> {spacing} ratio (PX fixed point).
	static unsigned char justificationFromProps(const librevenge::RVNGPropertyList &propList);
	static unsigned long lineSpacingToPX(const std::string &lineHeight);

	// ODT font family + point size -> MacFont (canonical classic name/ID)
	static MacFont macFontFor(const std::string &family);
	// emit a $D1/$01 Set Text Font change: oldFont -> target
	void emitFontChange(const MacFont &oldFont, const MacFont &target);
	// emit a $D1/$02 Set Font Size change (integer points)
	void emitFontSizeChange(int oldPoints, int targetPoints);
	// emit a $D1/$00 Set Text Color change (16-bit RGB components)
	void emitColorChange(unsigned oldRGB, unsigned rgb);
	// append the WP4 representation of a single Unicode code point
	void appendCodePoint(unsigned long cp);

	// paragraph indentation: $C2/<def>{adj}<C2>; def 0 = indent, 1 = left+right
	void emitIndent(unsigned char def, double adjInches);

	// map an ODF color "#rrggbb" to 0xRRGGBB (fallback on failure)
	static unsigned parseHexColor(const std::string &s, unsigned fallback);
	// ODF character formatting -> the list of WP4 attribute codes
	static std::vector<unsigned char> attrsFromProps(const librevenge::RVNGPropertyList &propList);

	// header/footer sub-document machinery
	void beginHeaderFooter(unsigned char subGroup, const librevenge::RVNGPropertyList &propList);
	void endHeaderFooter();
	// footnote/endnote sub-document machinery
	void beginNote(bool endnote, const librevenge::RVNGPropertyList &numberProps);
	void endNote();

	std::string m_outputFileName;
	std::vector<unsigned char> m_body;    // the WP4 document area
	std::vector<unsigned char> m_subDoc;  // scratch buffer for the current sub-doc
	std::vector<unsigned char> *m_out;    // where emission currently goes

	// --- header/footer (group $D5) state ---
	unsigned char m_hfSubGroup;
	bool m_hfHasPageNumber;               // does this header/footer hold a page #?
	std::string m_hfOccurrenceStr;        // "all", "even", "odd" from open{Header,Footer}
	// main-document state saved while a sub-document is being emitted
	MacFont m_savedFont;
	int m_savedFontSize;
	unsigned m_savedColor;
	unsigned char m_savedJustification;
	unsigned long m_savedSpacing;
	std::vector<std::vector<unsigned char> > m_savedAttributeStack;

	// --- footnote/endnote (group $D6) state ---
	bool m_noteIsEndnote;
	unsigned m_noteNumber;
	unsigned m_footnoteCounter;
	unsigned m_endnoteCounter;
	bool m_noteFirstParaPending;   // suppress the note's hanging indent once
	bool m_skipNextTab;            // drop the note body's own leading tab

	// --- tables ($E2/$01 + $DC EOL types) state ---
	bool m_inCell;                 // inside a table cell
	unsigned m_currentColumn;      // next column index in the current row
	unsigned m_tableNumColumns;
	unsigned m_tableNumRows;       // total rows (from the table property list)
	unsigned m_tableRowCount;      // rows opened so far
	bool m_tableOffDone;           // $DC/$1A emitted for the current table
	unsigned m_tableIdCounter;     // unique $E2/$01 [Table ID2] per table

	// --- list / paragraph-numbering state ---
	int m_listLevel;               // current 0-based level, -1 when not in a list
	bool m_outlineOn;              // $D2/$02 numbering defs emitted (first list)
	bool m_listOrdered[8];
	char m_listFormat[8];          // '1','a','A','i','I'
	std::string m_listSuffix[8];
	std::string m_listPrefix[8];
	int m_listCounter[8];
	bool m_pendingBullet;          // insert the bullet at the paragraph open
	bool m_pendingNumberSpace;     // space after an auto paragraph number
	double m_pendingIndentInches;  // $C2 indent deferred to the paragraph open
	void emitParagraphNumberDefinition(const char levelFormat[8],
	                                   const std::string levelSuffix[8]);

	// --- body formatting state (old->new values are emitted per change) ---
	MacFont m_currentFont;         // Mac font active in the text
	int m_currentFontSize;         // points
	unsigned m_currentColor;       // 0xRRGGBB
	std::vector<MacFont> m_fontStack;
	std::vector<int> m_fontSizeStack;
	std::vector<unsigned> m_colorStack;
	std::vector<std::vector<unsigned char> > m_attributeStack;

	unsigned char m_pageNumPos;    // current $D0/$08 page number position

	// justification (0 left, 1 center, 2 right, 3 full) and line spacing
	// (PX fixed-point ratio; $00010000 = single) persist until changed.
	unsigned char m_currentJustification;
	unsigned long m_currentSpacing;

	// paragraph margins. Targets arrive via openPageSpan; they are emitted
	// (inside $AA/$AB gates) at the next openParagraph while emitting the
	// main body, because non-character functions must sit in paragraph gates.
	double m_marginLeftT, m_marginRightT, m_marginTopT, m_marginBottomT; // inches
	double m_marginLeft, m_marginRight, m_marginTop, m_marginBottom;    // applied
	bool m_marginDirty;

	WP4Generator(const WP4Generator &) = delete;
	WP4Generator &operator=(const WP4Generator &) = delete;
};

#endif // WP4GENERATOR_H
