/**
 * \file InsetIndex.cpp
 * This file is part of LyX, the document processor.
 * Licence details can be found in the file COPYING.
 *
 * \author Lars Gullik Bjønnes
 * \author Jürgen Spitzmüller
 *
 * Full author contact details are available in file CREDITS.
 */
#include <config.h>

#include "InsetIndex.h"

#include "Buffer.h"
#include "BufferParams.h"
#include "BufferView.h"
#include "ColorSet.h"
#include "Cursor.h"
#include "DispatchResult.h"
#include "Encoding.h"
#include "FuncRequest.h"
#include "FuncStatus.h"
#include "IndicesList.h"
#include "Language.h"
#include "LaTeXFeatures.h"
#include "Lexer.h"
#include "output_latex.h"
#include "output_xhtml.h"
#include "xml.h"
#include "texstream.h"
#include "TextClass.h"
#include "TocBackend.h"

#include "support/debug.h"
#include "support/docstream.h"
#include "support/FileName.h"
#include "support/gettext.h"
#include "support/lstrings.h"

#include "frontends/alert.h"

#include <algorithm>
#include <set>
#include <ostream>

#include <QThreadStorage>

using namespace std;
using namespace lyx::support;

namespace lyx {

/////////////////////////////////////////////////////////////////////
//
// InsetIndex
//
///////////////////////////////////////////////////////////////////////


InsetIndex::InsetIndex(Buffer * buf, InsetIndexParams const & params)
        : InsetCollapsible(buf), params_(params)
{}


void InsetIndex::latex(otexstream & ios, OutputParams const & runparams_in) const
{
	OutputParams runparams(runparams_in);
	runparams.inIndexEntry = true;

	otexstringstream os;

	if (buffer().masterBuffer()->params().use_indices && !params_.index.empty()
		&& params_.index != "idx") {
		os << "\\sindex[";
		os << escape(params_.index);
		os << "]{";
	} else {
		os << "\\index";
		os << '{';
	}

	// Get the LaTeX output from InsetText. We need to deconstruct this later
	// in order to check if we need to generate a sorting key
	odocstringstream ourlatex;
	otexstream ots(ourlatex);
	InsetText::latex(ots, runparams);
	if (runparams.find_effective()) {
		// No need for special handling, if we are only searching for some patterns
		os << ourlatex.str() << "}";
		return;
	}

	// For the sorting key, we use the plaintext version
	odocstringstream ourplain;
	InsetText::plaintext(ourplain, runparams);

	// These are the LaTeX and plaintext representations
	docstring latexstr = ourlatex.str();
	docstring plainstr = ourplain.str();

	// This will get what follows | if anything does,
	// the command (e.g., see, textbf) for pagination
	// formatting
	docstring cmd;

	// Check for the | separator to strip the cmd.
	// This goes wrong on an escaped "|", but as the escape
	// character can be changed in style files, we cannot
	// prevent that.
	size_t pos = latexstr.find(from_ascii("|"));
	if (pos != docstring::npos) {
		// Put the bit after "|" into cmd...
		cmd = latexstr.substr(pos + 1);
		// ...and erase that stuff from latexstr
		latexstr = latexstr.erase(pos);
		// ...as well as from plainstr
		size_t ppos = plainstr.find(from_ascii("|"));
		if (ppos < plainstr.size())
			plainstr.erase(ppos);
		else
			LYXERR0("The `|' separator was not found in the plaintext version!");
	}

	// Separate the entries and subentries, i.e., split on "!".
	// This goes wrong on an escaped "!", but as the escape
	// character can be changed in style files, we cannot
	// prevent that.
	std::vector<docstring> const levels =
			getVectorFromString(latexstr, from_ascii("!"), true);
	std::vector<docstring> const levels_plain =
			getVectorFromString(plainstr, from_ascii("!"), true);

	vector<docstring>::const_iterator it = levels.begin();
	vector<docstring>::const_iterator end = levels.end();
	vector<docstring>::const_iterator it2 = levels_plain.begin();
	bool first = true;
	for (; it != end; ++it) {
		// The separator needs to be put back when
		// writing the levels, except for the first level
		if (!first)
			os << '!';
		else
			first = false;

		// Now here comes the reason for this whole procedure:
		// We try to correctly sort macros and formatted strings.
		// If we find a command, prepend a plain text
		// version of the content to get sorting right,
		// e.g. \index{LyX@\LyX}, \index{text@\textbf{text}}.
		// We do this on all levels.
		// We don't do it if the level already contains a '@', though.
		if (contains(*it, '\\') && !contains(*it, '@')) {
			// Plaintext might return nothing (e.g. for ERTs).
			// In that case, we use LaTeX.
			docstring const spart =
					(it2 < levels_plain.end() && !(*it2).empty())
					? *it2 : *it;
			// Now we need to validate that all characters in
			// the sorting part are representable in the current
			// encoding. If not try the LaTeX macro which might
			// or might not be a good choice, and issue a warning.
			pair<docstring, docstring> spart_latexed =
					runparams.encoding->latexString(spart, runparams.dryrun);
			if (!spart_latexed.second.empty())
				LYXERR0("Uncodable character in index entry. Sorting might be wrong!");
			if (spart != spart_latexed.first && !runparams.dryrun) {
				// FIXME: warning should be passed to the error dialog
				frontend::Alert::warning(_("Index sorting failed"),
							 bformat(_("LyX's automatic index sorting algorithm faced\n"
								   "problems with the entry '%1$s'.\n"
								   "Please specify the sorting of this entry manually, as\n"
								   "explained in the User Guide."), spart));
			}
			// Remove remaining \'s from the sort key
			docstring ppart = subst(spart_latexed.first, from_ascii("\\"), docstring());
			// Plain quotes need to be escaped, however (#10649), as this
			// is the default escape character
			ppart = subst(ppart, from_ascii("\""), from_ascii("\\\""));

			// Now insert the sortkey, separated by '@'.
			os << ppart;
			os << '@';
		}
		// Insert the actual level text
		docstring const tpart = *it;
		os << tpart;
		if (it2 < levels_plain.end())
			++it2;
	}
	// At last, re-insert the command, separated by "|"
	if (!cmd.empty()) {
		os << "|" << cmd;
	}
	os << '}';

	// In macros with moving arguments, such as \section,
	// we store the index and output it after the macro (#2154)
	if (runparams_in.postpone_fragile_stuff)
		runparams_in.post_macro += os.str();
	else
		ios << os.release();
}


void InsetIndex::docbook(XMLStream & xs, OutputParams const & runparams) const
{
	// Get the content of the inset as LaTeX, as some things may be encoded as ERT (like {}).
	// TODO: if there is an ERT within the index term, its conversion should be tried, in case it becomes useful;
	//  otherwise, ERTs should become comments. For now, they are just copied as-is, which is barely satisfactory.
	odocstringstream odss;
	otexstream ots(odss);
	InsetText::latex(ots, runparams);
	docstring latexString = trim(odss.str());

	// Check whether there are unsupported things. @ is supported, but only for sorting, without specific formatting.
	if (latexString.find(from_utf8("@\\")) != lyx::docstring::npos) {
		docstring error = from_utf8("Unsupported feature: an index entry contains an @\\. "
									"Complete entry: \"") + latexString + from_utf8("\"");
		LYXERR0(error);
		xs << XMLStream::ESCAPE_NONE << (from_utf8("<!-- Output Error: ") + error + from_utf8(" -->\n"));
	}

	// Handle several indices (indicated in the inset instead of the raw latexString).
	docstring indexType = from_utf8("");
	if (buffer().masterBuffer()->params().use_indices) {
		indexType += " type=\"" + params_.index + "\"";
	}

	// Split the string into its main constituents: terms, and command (see, see also, range).
	size_t positionVerticalBar = latexString.find(from_ascii("|")); // What comes before | is (sub)(sub)entries.
	docstring indexTerms = latexString.substr(0, positionVerticalBar);
	docstring command;
	if (positionVerticalBar != lyx::docstring::npos) {
		command =  latexString.substr(positionVerticalBar + 1);
	}

	// Handle sorting issues, with @.
	vector<docstring> sortingElements = getVectorFromString(indexTerms, from_ascii("@"), false);
	docstring sortAs;
	if (sortingElements.size() == 2) {
		sortAs = sortingElements[0];
		indexTerms = sortingElements[1];
	}

	// Handle primary, secondary, and tertiary terms (entries, subentries, and subsubentries, for LaTeX).
	vector<docstring> terms = getVectorFromString(indexTerms, from_ascii("!"), false);

	// Handle ranges. Happily, (| and |) can only be at the end of the string!
	bool hasStartRange = latexString.find(from_ascii("|(")) != lyx::docstring::npos;
	bool hasEndRange = latexString.find(from_ascii("|)")) != lyx::docstring::npos;
	if (hasStartRange || hasEndRange) {
		// Remove the ranges from the command if they do not appear at the beginning.
		size_t index = 0;
		while ((index = command.find(from_utf8("|("), index)) != std::string::npos)
			command.erase(index, 1);
		index = 0;
		while ((index = command.find(from_utf8("|)"), index)) != std::string::npos)
			command.erase(index, 1);

		// Remove the ranges when they are the only vertical bar in the complete string.
		if (command[0] == '(' || command[0] == ')')
			command.erase(0, 1);
	}

	// Handle see and seealso. As "see" is a prefix of "seealso", the order of the comparisons is important.
	// Both commands are mutually exclusive!
	docstring see = from_utf8("");
	vector<docstring> seeAlsoes;
	if (command.substr(0, 3) == "see") {
		// Unescape brackets.
		size_t index = 0;
		while ((index = command.find(from_utf8("\\{"), index)) != std::string::npos)
			command.erase(index, 1);
		index = 0;
		while ((index = command.find(from_utf8("\\}"), index)) != std::string::npos)
			command.erase(index, 1);

		// Retrieve the part between brackets, and remove the complete seealso.
		size_t positionOpeningBracket = command.find(from_ascii("{"));
		size_t positionClosingBracket = command.find(from_ascii("}"));
		docstring list = command.substr(positionOpeningBracket + 1, positionClosingBracket - positionOpeningBracket - 1);

		// Parse the list of referenced entries (or a single one for see).
		if (command.substr(0, 7) == "seealso") {
			seeAlsoes = getVectorFromString(list, from_ascii(","), false);
		} else {
			see = list;

			if (see.find(from_ascii(",")) != std::string::npos) {
				docstring error = from_utf8("Several index terms found as \"see\"! Only one is acceptable. "
											"Complete entry: \"") + latexString + from_utf8("\"");
				LYXERR0(error);
				xs << XMLStream::ESCAPE_NONE << (from_utf8("<!-- Output Error: ") + error + from_utf8(" -->\n"));
			}
		}

		// Remove the complete see/seealso from the commands, in case there is something else to parse.
		command = command.substr(positionClosingBracket + 1);
	}

	// Some parts of the strings are not parsed, as they do not have anything matching in DocBook: things like
	// formatting the entry or the page number, other strings for sorting. https://wiki.lyx.org/Tips/Indexing
	// If there are such things in the index entry, then this code may miserably fail. For example, for "Peter|(textbf",
	// no range will be detected.
	// TODO: Could handle formatting as significance="preferred"?
	if (!command.empty()) {
		docstring error = from_utf8("Unsupported feature: an index entry contains a | with an unsupported command, ")
				          + command + from_utf8(". ") + from_utf8("Complete entry: \"") + latexString + from_utf8("\"");
		LYXERR0(error);
		xs << XMLStream::ESCAPE_NONE << (from_utf8("<!-- Output Error: ") + error + from_utf8(" -->\n"));
	}

    // Write all of this down.
	if (terms.empty() && !hasEndRange) {
		docstring error = from_utf8("No index term found! Complete entry: \"") + latexString + from_utf8("\"");
		LYXERR0(error);
		xs << XMLStream::ESCAPE_NONE << (from_utf8("<!-- Output Error: ") + error + from_utf8(" -->\n"));
	} else {
		// Generate the attributes for ranges. It is based on the terms that are indexed, but the ID must be unique
		// to this indexing area (xml::cleanID does not guarantee this: for each call with the same arguments,
		// the same legal ID is produced; here, as the input would be the same, the output must be, by design).
		// Hence the thread-local storage, as the numbers must strictly be unique, and thus cannot be shared across
		// a paragraph (making the solution used for HTML worthless). This solution is very similar to the one used in
		// xml::cleanID.
		// indexType can only be used for singular and startofrange types!
		docstring attrs;
		if (!hasStartRange && !hasEndRange) {
			attrs = indexType;
		} else {
			// Append an ID if uniqueness is not guaranteed across the document.
			static QThreadStorage<set<docstring>> tKnownTermLists;
			static QThreadStorage<int> tID;

			set<docstring> &knownTermLists = tKnownTermLists.localData();
			int &ID = tID.localData();

			if (!tID.hasLocalData()) {
				tID.localData() = 0;
			}

			// Modify the index terms to add the unique ID if needed.
			docstring newIndexTerms = indexTerms;
			if (knownTermLists.find(indexTerms) != knownTermLists.end()) {
				newIndexTerms += from_ascii(string("-") + to_string(ID));

				// Only increment for the end of range, so that the same number is used for the start of range.
				if (hasEndRange) {
					ID++;
				}
			}

			// Term list not yet known: add it to the set AFTER the end of range. After
			if (knownTermLists.find(indexTerms) == knownTermLists.end() && hasEndRange) {
				knownTermLists.insert(indexTerms);
			}

			// Generate the attributes.
			docstring id = xml::cleanID(newIndexTerms);
			if (hasStartRange) {
				attrs = indexType + " class=\"startofrange\" xml:id=\"" + id + "\"";
			} else {
				attrs = " class=\"endofrange\" startref=\"" + id + "\"";
			}
		}

		// Handle the index terms (including the specific index for this entry).
		if (hasEndRange) {
			xs << xml::CompTag("indexterm", attrs);
		} else {
			xs << xml::StartTag("indexterm", attrs);
			if (!terms.empty()) { // hasEndRange has no content.
				docstring attr;
				if (!sortAs.empty()) {
					attr = from_utf8("sortas='") + sortAs + from_utf8("'");
				}

				xs << xml::StartTag("primary", attr);
				xs << terms[0];
				xs << xml::EndTag("primary");
			}
			if (terms.size() > 1) {
				xs << xml::StartTag("secondary");
				xs << terms[1];
				xs << xml::EndTag("secondary");
			}
			if (terms.size() > 2) {
				xs << xml::StartTag("tertiary");
				xs << terms[2];
				xs << xml::EndTag("tertiary");
			}

			// Handle see and see also.
			if (!see.empty()) {
				xs << xml::StartTag("see");
				xs << see;
				xs << xml::EndTag("see");
			}

			if (!seeAlsoes.empty()) {
				for (auto &entry : seeAlsoes) {
					xs << xml::StartTag("seealso");
					xs << entry;
					xs << xml::EndTag("seealso");
				}
			}

			// Close the entry.
			xs << xml::EndTag("indexterm");
		}
	}
}


docstring InsetIndex::xhtml(XMLStream & xs, OutputParams const &) const
{
	// we just print an anchor, taking the paragraph ID from
	// our own interior paragraph, which doesn't get printed
	std::string const magic = paragraphs().front().magicLabel();
	std::string const attr = "id='" + magic + "'";
	xs << xml::CompTag("a", attr);
	return docstring();
}


bool InsetIndex::showInsetDialog(BufferView * bv) const
{
	bv->showDialog("index", params2string(params_),
			const_cast<InsetIndex *>(this));
	return true;
}


void InsetIndex::doDispatch(Cursor & cur, FuncRequest & cmd)
{
	switch (cmd.action()) {

	case LFUN_INSET_MODIFY: {
		if (cmd.getArg(0) == "changetype") {
			cur.recordUndoInset(this);
			params_.index = from_utf8(cmd.getArg(1));
			break;
		}
		InsetIndexParams params;
		InsetIndex::string2params(to_utf8(cmd.argument()), params);
		cur.recordUndoInset(this);
		params_.index = params.index;
		// what we really want here is a TOC update, but that means
		// a full buffer update
		cur.forceBufferUpdate();
		break;
	}

	case LFUN_INSET_DIALOG_UPDATE:
		cur.bv().updateDialog("index", params2string(params_));
		break;

	default:
		InsetCollapsible::doDispatch(cur, cmd);
		break;
	}
}


bool InsetIndex::getStatus(Cursor & cur, FuncRequest const & cmd,
		FuncStatus & flag) const
{
	switch (cmd.action()) {

	case LFUN_INSET_MODIFY:
		if (cmd.getArg(0) == "changetype") {
			docstring const newtype = from_utf8(cmd.getArg(1));
			Buffer const & realbuffer = *buffer().masterBuffer();
			IndicesList const & indiceslist = realbuffer.params().indiceslist();
			Index const * index = indiceslist.findShortcut(newtype);
			flag.setEnabled(index != 0);
			flag.setOnOff(
				from_utf8(cmd.getArg(1)) == params_.index);
			return true;
		}
		return InsetCollapsible::getStatus(cur, cmd, flag);

	case LFUN_INSET_DIALOG_UPDATE: {
		Buffer const & realbuffer = *buffer().masterBuffer();
		flag.setEnabled(realbuffer.params().use_indices);
		return true;
	}

	default:
		return InsetCollapsible::getStatus(cur, cmd, flag);
	}
}


ColorCode InsetIndex::labelColor() const
{
	if (params_.index.empty() || params_.index == from_ascii("idx"))
		return InsetCollapsible::labelColor();
	// FIXME UNICODE
	ColorCode c = lcolor.getFromLyXName(to_utf8(params_.index)
					    + "@" + buffer().fileName().absFileName());
	if (c == Color_none)
		c = InsetCollapsible::labelColor();
	return c;
}


docstring InsetIndex::toolTip(BufferView const &, int, int) const
{
	docstring tip = _("Index Entry");
	if (buffer().params().use_indices && !params_.index.empty()) {
		Buffer const & realbuffer = *buffer().masterBuffer();
		IndicesList const & indiceslist = realbuffer.params().indiceslist();
		tip += " (";
		Index const * index = indiceslist.findShortcut(params_.index);
		if (!index)
			tip += _("unknown type!");
		else
			tip += index->index();
		tip += ")";
	}
	tip += ": ";
	return toolTipText(tip);
}


docstring const InsetIndex::buttonLabel(BufferView const & bv) const
{
	InsetLayout const & il = getLayout();
	docstring label = translateIfPossible(il.labelstring());

	if (buffer().params().use_indices && !params_.index.empty()) {
		Buffer const & realbuffer = *buffer().masterBuffer();
		IndicesList const & indiceslist = realbuffer.params().indiceslist();
		label += " (";
		Index const * index = indiceslist.findShortcut(params_.index);
		if (!index)
			label += _("unknown type!");
		else
			label += index->index();
		label += ")";
	}

	if (!il.contentaslabel() || geometry(bv) != ButtonOnly)
		return label;
	return getNewLabel(label);
}


void InsetIndex::write(ostream & os) const
{
	os << to_utf8(layoutName());
	params_.write(os);
	InsetCollapsible::write(os);
}


void InsetIndex::read(Lexer & lex)
{
	params_.read(lex);
	InsetCollapsible::read(lex);
}


string InsetIndex::params2string(InsetIndexParams const & params)
{
	ostringstream data;
	data << "index";
	params.write(data);
	return data.str();
}


void InsetIndex::string2params(string const & in, InsetIndexParams & params)
{
	params = InsetIndexParams();
	if (in.empty())
		return;

	istringstream data(in);
	Lexer lex;
	lex.setStream(data);
	lex.setContext("InsetIndex::string2params");
	lex >> "index";
	params.read(lex);
}


void InsetIndex::addToToc(DocIterator const & cpit, bool output_active,
						  UpdateType utype, TocBackend & backend) const
{
	DocIterator pit = cpit;
	pit.push_back(CursorSlice(const_cast<InsetIndex &>(*this)));
	docstring str;
	string type = "index";
	if (buffer().masterBuffer()->params().use_indices)
		type += ":" + to_utf8(params_.index);
	// this is unlikely to be terribly long
	text().forOutliner(str, INT_MAX);
	TocBuilder & b = backend.builder(type);
	b.pushItem(pit, str, output_active);
	// Proceed with the rest of the inset.
	InsetCollapsible::addToToc(cpit, output_active, utype, backend);
	b.pop();
}


void InsetIndex::validate(LaTeXFeatures & features) const
{
	if (buffer().masterBuffer()->params().use_indices
	    && !params_.index.empty()
	    && params_.index != "idx")
		features.require("splitidx");
	InsetCollapsible::validate(features);
}


string InsetIndex::contextMenuName() const
{
	return "context-index";
}


bool InsetIndex::hasSettings() const
{
	return buffer().masterBuffer()->params().use_indices;
}




/////////////////////////////////////////////////////////////////////
//
// InsetIndexParams
//
///////////////////////////////////////////////////////////////////////


void InsetIndexParams::write(ostream & os) const
{
	os << ' ';
	if (!index.empty())
		os << to_utf8(index);
	else
		os << "idx";
	os << '\n';
}


void InsetIndexParams::read(Lexer & lex)
{
	if (lex.eatLine())
		index = lex.getDocString();
	else
		index = from_ascii("idx");
}


/////////////////////////////////////////////////////////////////////
//
// InsetPrintIndex
//
///////////////////////////////////////////////////////////////////////

InsetPrintIndex::InsetPrintIndex(Buffer * buf, InsetCommandParams const & p)
	: InsetCommand(buf, p)
{}


ParamInfo const & InsetPrintIndex::findInfo(string const & /* cmdName */)
{
	static ParamInfo param_info_;
	if (param_info_.empty()) {
		param_info_.add("type", ParamInfo::LATEX_OPTIONAL,
				ParamInfo::HANDLING_ESCAPE);
		param_info_.add("name", ParamInfo::LATEX_OPTIONAL,
				ParamInfo::HANDLING_LATEXIFY);
		param_info_.add("literal", ParamInfo::LYX_INTERNAL);
	}
	return param_info_;
}


docstring InsetPrintIndex::screenLabel() const
{
	bool const printall = suffixIs(getCmdName(), '*');
	bool const multind = buffer().masterBuffer()->params().use_indices;
	if ((!multind
	     && getParam("type") == from_ascii("idx"))
	    || (getParam("type").empty() && !printall))
		return _("Index");
	Buffer const & realbuffer = *buffer().masterBuffer();
	IndicesList const & indiceslist = realbuffer.params().indiceslist();
	Index const * index = indiceslist.findShortcut(getParam("type"));
	if (!index && !printall)
		return _("Unknown index type!");
	docstring res = printall ? _("All indexes") : index->index();
	if (!multind)
		res += " (" + _("non-active") + ")";
	else if (contains(getCmdName(), "printsubindex"))
		res += " (" + _("subindex") + ")";
	return res;
}


bool InsetPrintIndex::isCompatibleCommand(string const & s)
{
	return s == "printindex" || s == "printsubindex"
		|| s == "printindex*" || s == "printsubindex*";
}


void InsetPrintIndex::doDispatch(Cursor & cur, FuncRequest & cmd)
{
	switch (cmd.action()) {

	case LFUN_INSET_MODIFY: {
		if (cmd.argument() == from_ascii("toggle-subindex")) {
			string scmd = getCmdName();
			if (contains(scmd, "printindex"))
				scmd = subst(scmd, "printindex", "printsubindex");
			else
				scmd = subst(scmd, "printsubindex", "printindex");
			cur.recordUndo();
			setCmdName(scmd);
			break;
		} else if (cmd.argument() == from_ascii("check-printindex*")) {
			string scmd = getCmdName();
			if (suffixIs(scmd, '*'))
				break;
			scmd += '*';
			cur.recordUndo();
			setParam("type", docstring());
			setCmdName(scmd);
			break;
		}
		InsetCommandParams p(INDEX_PRINT_CODE);
		// FIXME UNICODE
		InsetCommand::string2params(to_utf8(cmd.argument()), p);
		if (p.getCmdName().empty()) {
			cur.noScreenUpdate();
			break;
		}
		cur.recordUndo();
		setParams(p);
		break;
	}

	default:
		InsetCommand::doDispatch(cur, cmd);
		break;
	}
}


bool InsetPrintIndex::getStatus(Cursor & cur, FuncRequest const & cmd,
	FuncStatus & status) const
{
	switch (cmd.action()) {

	case LFUN_INSET_MODIFY: {
		if (cmd.argument() == from_ascii("toggle-subindex")) {
			status.setEnabled(buffer().masterBuffer()->params().use_indices);
			status.setOnOff(contains(getCmdName(), "printsubindex"));
			return true;
		} else if (cmd.argument() == from_ascii("check-printindex*")) {
			status.setEnabled(buffer().masterBuffer()->params().use_indices);
			status.setOnOff(suffixIs(getCmdName(), '*'));
			return true;
		} if (cmd.getArg(0) == "index_print"
		    && cmd.getArg(1) == "CommandInset") {
			InsetCommandParams p(INDEX_PRINT_CODE);
			InsetCommand::string2params(to_utf8(cmd.argument()), p);
			if (suffixIs(p.getCmdName(), '*')) {
				status.setEnabled(true);
				status.setOnOff(false);
				return true;
			}
			Buffer const & realbuffer = *buffer().masterBuffer();
			IndicesList const & indiceslist =
				realbuffer.params().indiceslist();
			Index const * index = indiceslist.findShortcut(p["type"]);
			status.setEnabled(index != 0);
			status.setOnOff(p["type"] == getParam("type"));
			return true;
		} else
			return InsetCommand::getStatus(cur, cmd, status);
	}

	case LFUN_INSET_DIALOG_UPDATE: {
		status.setEnabled(buffer().masterBuffer()->params().use_indices);
		return true;
	}

	default:
		return InsetCommand::getStatus(cur, cmd, status);
	}
}


void InsetPrintIndex::updateBuffer(ParIterator const &, UpdateType, bool const /*deleted*/)
{
	Index const * index =
		buffer().masterParams().indiceslist().findShortcut(getParam("type"));
	if (index)
		setParam("name", index->index());
}


void InsetPrintIndex::latex(otexstream & os, OutputParams const & runparams_in) const
{
	if (!buffer().masterBuffer()->params().use_indices) {
		if (getParam("type") == from_ascii("idx"))
			os << "\\printindex" << termcmd;
		return;
	}
	OutputParams runparams = runparams_in;
	os << getCommand(runparams);
}


void InsetPrintIndex::validate(LaTeXFeatures & features) const
{
	features.require("makeidx");
	if (buffer().masterBuffer()->params().use_indices)
		features.require("splitidx");
	InsetCommand::validate(features);
}


string InsetPrintIndex::contextMenuName() const
{
	return buffer().masterBuffer()->params().use_indices ?
		"context-indexprint" : string();
}


bool InsetPrintIndex::hasSettings() const
{
	return buffer().masterBuffer()->params().use_indices;
}


namespace {

void parseItem(docstring & s, bool for_output)
{
	// this does not yet check for escaped things
	size_type loc = s.find(from_ascii("@"));
	if (loc != string::npos) {
		if (for_output)
			s.erase(0, loc + 1);
		else
			s.erase(loc);
	}
	loc = s.find(from_ascii("|"));
	if (loc != string::npos)
		s.erase(loc);
}


void extractSubentries(docstring const & entry, docstring & main,
		docstring & sub1, docstring & sub2)
{
	if (entry.empty())
		return;
	size_type const loc = entry.find(from_ascii(" ! "));
	if (loc == string::npos)
		main = entry;
	else {
		main = trim(entry.substr(0, loc));
		size_t const locend = loc + 3;
		size_type const loc2 = entry.find(from_ascii(" ! "), locend);
		if (loc2 == string::npos) {
			sub1 = trim(entry.substr(locend));
		} else {
			sub1 = trim(entry.substr(locend, loc2 - locend));
			sub2 = trim(entry.substr(loc2 + 3));
		}
	}
}


struct IndexEntry
{
	IndexEntry()
	{}

	IndexEntry(docstring const & s, DocIterator const & d)
			: dit(d)
	{
		extractSubentries(s, main, sub, subsub);
		parseItem(main, false);
		parseItem(sub, false);
		parseItem(subsub, false);
	}

	bool equal(IndexEntry const & rhs) const
	{
		return main == rhs.main && sub == rhs.sub && subsub == rhs.subsub;
	}

	bool same_sub(IndexEntry const & rhs) const
	{
		return main == rhs.main && sub == rhs.sub;
	}

	bool same_main(IndexEntry const & rhs) const
	{
		return main == rhs.main;
	}

	docstring main;
	docstring sub;
	docstring subsub;
	DocIterator dit;
};

bool operator<(IndexEntry const & lhs, IndexEntry const & rhs)
{
	int comp = compare_no_case(lhs.main, rhs.main);
	if (comp == 0)
		comp = compare_no_case(lhs.sub, rhs.sub);
	if (comp == 0)
		comp = compare_no_case(lhs.subsub, rhs.subsub);
	return (comp < 0);
}

} // namespace


docstring InsetPrintIndex::xhtml(XMLStream &, OutputParams const & op) const
{
	BufferParams const & bp = buffer().masterBuffer()->params();

	// we do not presently support multiple indices, so we refuse to print
	// anything but the main index, so as not to generate multiple indices.
	// NOTE Multiple index support would require some work. The reason
	// is that the TOC does not know about multiple indices. Either it would
	// need to be told about them (not a bad idea), or else the index entries
	// would need to be collected differently, say, during validation.
	if (bp.use_indices && getParam("type") != from_ascii("idx"))
		return docstring();

	shared_ptr<Toc const> toc = buffer().tocBackend().toc("index");
	if (toc->empty())
		return docstring();

	// Collect the index entries in a form we can use them.
	Toc::const_iterator it = toc->begin();
	Toc::const_iterator const en = toc->end();
	vector<IndexEntry> entries;
	for (; it != en; ++it)
		if (it->isOutput())
			entries.push_back(IndexEntry(it->str(), it->dit()));

	if (entries.empty())
		// not very likely that all the index entries are in notes or
		// whatever, but....
		return docstring();

	stable_sort(entries.begin(), entries.end());

	Layout const & lay = bp.documentClass().htmlTOCLayout();
	string const & tocclass = lay.defaultCSSClass();
	string const tocattr = "class='index " + tocclass + "'";

	// we'll use our own stream, because we are going to defer everything.
	// that's how we deal with the fact that we're probably inside a standard
	// paragraph, and we don't want to be.
	odocstringstream ods;
	XMLStream xs(ods);

	xs << xml::StartTag("div", tocattr);
	xs << xml::StartTag(lay.htmltag(), lay.htmlattr())
		 << translateIfPossible(from_ascii("Index"),
	                          op.local_font->language()->lang())
		 << xml::EndTag(lay.htmltag());
	xs << xml::StartTag("ul", "class='main'");
	Font const dummy;

	vector<IndexEntry>::const_iterator eit = entries.begin();
	vector<IndexEntry>::const_iterator const een = entries.end();
	// tracks whether we are already inside a main entry (1),
	// a sub-entry (2), or a sub-sub-entry (3). see below for the
	// details.
	int level = 1;
	// the last one we saw
	IndexEntry last;
	int entry_number = -1;
	for (; eit != een; ++eit) {
		Paragraph const & par = eit->dit.innerParagraph();
		if (entry_number == -1 || !eit->equal(last)) {
			if (entry_number != -1) {
				// not the first time through the loop, so
				// close last entry or entries, depending.
				if (level == 3) {
					// close this sub-sub-entry
					xs << xml::EndTag("li") << xml::CR();
					// is this another sub-sub-entry within the same sub-entry?
					if (!eit->same_sub(last)) {
						// close this level
						xs << xml::EndTag("ul") << xml::CR();
						level = 2;
					}
				}
				// the point of the second test here is that we might get
				// here two ways: (i) by falling through from above; (ii) because,
				// though the sub-entry hasn't changed, the sub-sub-entry has,
				// which means that it is the first sub-sub-entry within this
				// sub-entry. In that case, we do not want to close anything.
				if (level == 2 && !eit->same_sub(last)) {
					// close sub-entry
					xs << xml::EndTag("li") << xml::CR();
					// is this another sub-entry with the same main entry?
					if (!eit->same_main(last)) {
						// close this level
						xs << xml::EndTag("ul") << xml::CR();
						level = 1;
					}
				}
				// again, we can get here two ways: from above, or because we have
				// found the first sub-entry. in the latter case, we do not want to
				// close the entry.
				if (level == 1 && !eit->same_main(last)) {
					// close entry
					xs << xml::EndTag("li") << xml::CR();
				}
			}

			// we'll be starting new entries
			entry_number = 0;

			// We need to use our own stream, since we will have to
			// modify what we get back.
			odocstringstream ent;
			XMLStream entstream(ent);
			OutputParams ours = op;
			ours.for_toc = true;
			par.simpleLyXHTMLOnePar(buffer(), entstream, ours, dummy);

			// these will contain XHTML versions of the main entry, etc
			// remember that everything will already have been escaped,
			// so we'll need to use NextRaw() during output.
			docstring main;
			docstring sub;
			docstring subsub;
			extractSubentries(ent.str(), main, sub, subsub);
			parseItem(main, true);
			parseItem(sub, true);
			parseItem(subsub, true);

			if (level == 3) {
				// another subsubentry
				xs << xml::StartTag("li", "class='subsubentry'")
				   << XMLStream::ESCAPE_NONE << subsub;
			} else if (level == 2) {
				// there are two ways we can be here:
				// (i) we can actually be inside a sub-entry already and be about
				//     to output the first sub-sub-entry. in this case, our sub
				//     and the last sub will be the same.
				// (ii) we can just have closed a sub-entry, possibly after also
				//     closing a list of sub-sub-entries. here our sub and the last
				//     sub are different.
				// only in the latter case do we need to output the new sub-entry.
				// note that in this case, too, though, the sub-entry might already
				// have a sub-sub-entry.
				if (eit->sub != last.sub)
					xs << xml::StartTag("li", "class='subentry'")
					   << XMLStream::ESCAPE_NONE << sub;
				if (!subsub.empty()) {
					// it's actually a subsubentry, so we need to start that list
					xs << xml::CR()
					   << xml::StartTag("ul", "class='subsubentry'")
					   << xml::StartTag("li", "class='subsubentry'")
					   << XMLStream::ESCAPE_NONE << subsub;
					level = 3;
				}
			} else {
				// there are also two ways we can be here:
				// (i) we can actually be inside an entry already and be about
				//     to output the first sub-entry. in this case, our main
				//     and the last main will be the same.
				// (ii) we can just have closed an entry, possibly after also
				//     closing a list of sub-entries. here our main and the last
				//     main are different.
				// only in the latter case do we need to output the new main entry.
				// note that in this case, too, though, the main entry might already
				// have a sub-entry, or even a sub-sub-entry.
				if (eit->main != last.main)
					xs << xml::StartTag("li", "class='main'") << main;
				if (!sub.empty()) {
					// there's a sub-entry, too
					xs << xml::CR()
					   << xml::StartTag("ul", "class='subentry'")
					   << xml::StartTag("li", "class='subentry'")
					   << XMLStream::ESCAPE_NONE << sub;
					level = 2;
					if (!subsub.empty()) {
						// and a sub-sub-entry
						xs << xml::CR()
						   << xml::StartTag("ul", "class='subsubentry'")
						   << xml::StartTag("li", "class='subsubentry'")
						   << XMLStream::ESCAPE_NONE << subsub;
						level = 3;
					}
				}
			}
		}
		// finally, then, we can output the index link itself
		string const parattr = "href='#" + par.magicLabel() + "'";
		xs << (entry_number == 0 ? ":" : ",");
		xs << " " << xml::StartTag("a", parattr)
		   << ++entry_number << xml::EndTag("a");
		last = *eit;
	}
	// now we have to close all the open levels
	while (level > 0) {
		xs << xml::EndTag("li") << xml::EndTag("ul") << xml::CR();
		--level;
	}
	xs << xml::EndTag("div") << xml::CR();
	return ods.str();
}

} // namespace lyx
