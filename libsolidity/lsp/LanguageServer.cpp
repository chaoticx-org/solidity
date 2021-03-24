/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>
#include <libsolidity/interface/ReadFile.h>
#include <libsolidity/lsp/LanguageServer.h>
#include <libsolidity/lsp/ReferenceCollector.h>

#include <liblangutil/SourceReferenceExtractor.h>
#include <liblangutil/CharStream.h>

#include <libsolutil/Visitor.h>
#include <libsolutil/JSON.h>

#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <fmt/format.h>

#include <ostream>
#include <string>

using namespace std;
using namespace std::placeholders;

using namespace solidity::langutil;
using namespace solidity::frontend;

namespace solidity::lsp {

namespace // {{{ helpers
{

string toFileURI(boost::filesystem::path const& _path)
{
	return "file://" + _path.generic_string();
}

class ASTNodeLocator: public ASTConstVisitor
{
public:
	static ASTNode const* locate(int _pos, SourceUnit const& _sourceUnit)
	{
		ASTNodeLocator locator{_pos};
		_sourceUnit.accept(locator);
		return locator.m_closestMatch;
	}

	bool visitNode(ASTNode const& _node) override
	{
		if (_node.location().contains(m_pos))
		{
			m_closestMatch = &_node;
			return true;
		}
		return false;
	}

private:
	explicit ASTNodeLocator(int _pos): m_pos{_pos}, m_closestMatch{nullptr} {}

	int m_pos;
	ASTNode const* m_closestMatch;
};

optional<string> extractPathFromFileURI(std::string const& _uri)
{
	if (!boost::starts_with(_uri, "file://"))
		return nullopt;

	return _uri.substr(7);
}

Json::Value toJson(LineColumn _pos)
{
	Json::Value json = Json::objectValue;
	json["line"] = max(_pos.line, 0);
	json["character"] = max(_pos.column, 0);

	return json;
}

Json::Value toJsonRange(int _startLine, int _startColumn, int _endLine, int _endColumn)
{
	Json::Value json;
	json["start"] = toJson({_startLine, _startColumn});
	json["end"] = toJson({_endLine, _endColumn});
	return json;
}

Json::Value toJsonRange(SourceLocation const& _location)
{
	solAssert(_location.source, "");
	auto const [startLine, startColumn] = _location.source->translatePositionToLineColumn(_location.start);
	auto const [endLine, endColumn] = _location.source->translatePositionToLineColumn(_location.end);
	return toJsonRange(startLine, startColumn, endLine, endColumn);
}

Json::Value toJson(boost::filesystem::path const& _basePath, SourceLocation const& _location)
{
	solAssert(_location.source.get() != nullptr, "");
	Json::Value item = Json::objectValue;
	item["uri"] = toFileURI(_basePath / boost::filesystem::path(_location.source->name()));
	item["range"] = toJsonRange(_location);
	return item;
}

std::pair<size_t, size_t> offsetsOf(std::string const& _text, LineColumnRange _range) noexcept
{
	auto const start = CharStream::translateLineColumnToPosition(_text, _range.start.line, _range.start.column);
	auto const end = CharStream::translateLineColumnToPosition(_text, _range.end.line, _range.end.column);
	solAssert(start.has_value(), "");
	solAssert(end.has_value(), "");
	return std::pair{ size_t(start.value()), size_t(end.value()) };
}

} // }}} end helpers

LanguageServer::LanguageServer(Logger _logger, unique_ptr<Transport> _transport):
	m_client{move(_transport)},
	m_handlers{
		{"$/cancelRequest", {} }, // Don't do anything for now, as we're synchronous.
		{"cancelRequest", {} }, // Don't do anything for now, as we're synchronous.
		{"initialize", bind(&LanguageServer::handle_initialize, this, _1, _2)},
		{"initialized", {} },
		{"shutdown", [this](auto, auto) { m_shutdownRequested = true; }},
		{"textDocument/definition", [this](auto _id, auto _args) { handleGotoDefAndImpl(_id, _args); }},
		{"textDocument/didChange", bind(&LanguageServer::handle_textDocument_didChange, this, _1, _2)},
		{"textDocument/didClose", [](auto, auto) {/*nothing for now*/}},
		{"textDocument/didOpen", bind(&LanguageServer::handle_textDocument_didOpen, this, _1, _2)},
		{"textDocument/documentHighlight", bind(&LanguageServer::handle_textDocument_highlight, this, _1, _2)},
		{"textDocument/hover", bind(&LanguageServer::handle_textDocument_hover, this, _1, _2)},
		{"textDocument/implementation", [this](auto _id, auto _args) { handleGotoDefAndImpl(_id, _args); }},
		{"textDocument/references", bind(&LanguageServer::handle_textDocument_references, this, _1, _2)},
		{"workspace/didChangeConfiguration", bind(&LanguageServer::handle_workspace_didChangeConfiguration, this, _1, _2)},
	},
	m_logger{move(_logger)}
{
}

DocumentPosition LanguageServer::extractDocumentPosition(Json::Value const& _json) const
{
	DocumentPosition dpos{};

	dpos.path = extractPathFromFileURI(_json["textDocument"]["uri"].asString()).value();

	if (boost::algorithm::starts_with(dpos.path, m_basePath.generic_string()))
		dpos.path = dpos.path.substr(m_basePath.generic_string().size());

	dpos.position.line = _json["position"]["line"].asInt();
	dpos.position.column = _json["position"]["character"].asInt();

	return dpos;
}

void LanguageServer::changeConfiguration(Json::Value const& _settings)
{
	if (_settings["evm"].isString())
		if (auto const evmVersionOpt = EVMVersion::fromString(_settings["evm"].asString()); evmVersionOpt.has_value())
			m_evmVersion = evmVersionOpt.value();

	if (_settings["revertStrings"].isString())
		m_revertStrings = revertStringsFromString(_settings["revertStrings"].asString()).value_or(RevertStrings::Default);

	if (_settings["remapping"].isArray())
	{
		for (auto const& element: _settings["remapping"])
		{
			if (element.isString())
			{
				if (auto remappingOpt = ImportRemapper::parseRemapping(element.asString()); remappingOpt.has_value())
					m_remappings.emplace_back(move(remappingOpt.value()));
				else
					trace("Failed to parse remapping: '"s + element.asString() + "'");
			}
		}
	}

	static string_view constexpr strModelCheckerContracts = "model-checker-contracts";
	static string_view constexpr strModelCheckerEngine = "model-checker-engine";
	static string_view constexpr strModelCheckerTargets = "model-checker-targets";
	static string_view constexpr strModelCheckerTimeout = "model-checker-timeout";

	if (_settings[strModelCheckerContracts.data()].isString())
	{
		string contractsStr = _settings[strModelCheckerContracts.data()].as<string>();
		optional<ModelCheckerContracts> contracts = ModelCheckerContracts::fromString(contractsStr);
		if (contracts)
			m_modelCheckerSettings.contracts = move(*contracts);
		else
			log(fmt::format("Invalid option for {}: {}", strModelCheckerContracts, contractsStr));
	}

	if (_settings[strModelCheckerEngine.data()].isString())
	{
		string engineStr = _settings[strModelCheckerEngine.data()].as<string>();
		optional<ModelCheckerEngine> engine = ModelCheckerEngine::fromString(engineStr);
		if (engine)
			m_modelCheckerSettings.engine = *engine;
		else
			log(fmt::format("Invalid for {}: {}", strModelCheckerEngine, engineStr));
	}

	if (_settings[strModelCheckerTargets.data()].isString())
	{
		string targetsStr = _settings[strModelCheckerTargets.data()].as<string>();
		optional<ModelCheckerTargets> targets = ModelCheckerTargets::fromString(targetsStr);
		if (targets)
			m_modelCheckerSettings.targets = *targets;
		else
			log(fmt::format("Invalid for {}: {}", strModelCheckerTargets, targetsStr));
	}

	if (_settings[strModelCheckerTimeout.data()].isUInt())
		m_modelCheckerSettings.timeout = _settings[strModelCheckerTargets.data()].asUInt();
}

void LanguageServer::documentContentUpdated(string const& _path, LineColumnRange _range, string const& _replacementText)
{
	auto file = m_fileReader->sourceCodes().find(_path);
	if (file == m_fileReader->sourceCodes().end())
		return;

	string buffer = file->second;
	auto const [start, end] = offsetsOf(buffer, _range);
	buffer.replace(start, end - start, _replacementText);
	m_fileReader->setSource(_path, move(buffer));
}

void LanguageServer::documentContentUpdated(string const& _path, string const& _replacementText)
{
	auto file = m_fileReader->sourceCodes().find(_path);
	if (file == m_fileReader->sourceCodes().end())
		return;

	m_fileReader->setSource(_path, _replacementText);
	compileSource(_path);
}

constexpr DiagnosticSeverity toDiagnosticSeverity(Error::Type _errorType)
{
	using Type = Error::Type;
	using Severity = DiagnosticSeverity;
	switch (_errorType)
	{
		case Type::CodeGenerationError:
		case Type::DeclarationError:
		case Type::DocstringParsingError:
		case Type::ParserError:
		case Type::SyntaxError:
		case Type::TypeError:
			return Severity::Error;
		case Type::Warning:
			return Severity::Warning;
	}
	return Severity::Error; // Should never be reached.
}

bool LanguageServer::compile(std::string const& _path)
{
	// TODO: optimize! do not recompile if nothing has changed (file(s) not flagged dirty).

	auto const i = m_fileReader->sourceCodes().find(_path);
	if (i == m_fileReader->sourceCodes().end())
	{
		log("source code not found for path: " + _path);
		return false;
	}

	m_compilerStack.reset();
	m_compilerStack = make_unique<CompilerStack>(bind(&FileReader::readFile, ref(*m_fileReader), _1, _2));

	OptimiserSettings settings = OptimiserSettings::standard(); // TODO: get from config (might be useful for inspecting generated IR on hover)
	m_compilerStack->setOptimiserSettings(settings);
	m_compilerStack->setParserErrorRecovery(false);
	m_compilerStack->setRevertStringBehaviour(m_revertStrings);
	m_compilerStack->setSources(m_fileReader->sourceCodes());
	m_compilerStack->setRemappings(m_remappings);
	m_compilerStack->setEVMVersion(m_evmVersion);
	m_compilerStack->setModelCheckerSettings(m_modelCheckerSettings);

	m_compilerStack->compile(CompilerStack::State::AnalysisPerformed);
	return true;
}

void LanguageServer::compileSource(std::string const& _path)
{
	compile(_path);

	Json::Value params;
	params["uri"] = toFileURI(m_basePath / _path);

	params["diagnostics"] = Json::arrayValue;
	for (shared_ptr<Error const> const& error: m_compilerStack->errors())
	{
		SourceReferenceExtractor::Message const message = SourceReferenceExtractor::extract(*error);

		Json::Value jsonDiag;
		jsonDiag["source"] = "solc";
		jsonDiag["severity"] = int(toDiagnosticSeverity(error->type()));
		jsonDiag["message"] = message.primary.message;
		jsonDiag["range"] = toJsonRange(
			message.primary.position.line, message.primary.startColumn,
			message.primary.position.line, message.primary.endColumn
		);
		if (message.errorId.has_value())
			jsonDiag["code"] = Json::UInt64{message.errorId.value().error};

		for (SourceReference const& secondary: message.secondary)
		{
			Json::Value jsonRelated;
			jsonRelated["message"] = secondary.message;
			jsonRelated["location"]["uri"] = toFileURI(m_basePath / boost::filesystem::path(secondary.sourceName));
			jsonRelated["location"]["range"] = toJsonRange(
				secondary.position.line, secondary.startColumn,
				secondary.position.line, secondary.endColumn
			);
			jsonDiag["relatedInformation"].append(jsonRelated);
		}

		params["diagnostics"].append(jsonDiag);
	}

	m_client->notify("textDocument/publishDiagnostics", params);
}

frontend::ASTNode const* LanguageServer::findASTNode(LineColumn _position, std::string const& _fileName)
{
	if (!m_compilerStack || m_compilerStack->state() < frontend::CompilerStack::AnalysisPerformed)
		return nullptr;

	frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(_fileName);
	auto const sourcePos = sourceUnit.location().source->translateLineColumnToPosition(_position.line, _position.column);
	if (!sourcePos.has_value())
		return nullptr;

	return ASTNodeLocator::locate(sourcePos.value(), sourceUnit);
}

optional<SourceLocation> LanguageServer::declarationPosition(frontend::Declaration const* _declaration)
{
	if (!_declaration)
		return nullopt;

	if (_declaration->nameLocation().isValid())
		return _declaration->nameLocation();

	if (_declaration->location().isValid())
		return _declaration->location();

	return nullopt;
}

std::vector<SourceLocation> LanguageServer::findAllReferences(
	frontend::Declaration const* _declaration,
	string const& _sourceIdentifierName,
	frontend::SourceUnit const& _sourceUnit
)
{
	std::vector<SourceLocation> output;
	for (DocumentHighlight& highlight: ReferenceCollector::collect(_declaration, _sourceUnit, _sourceIdentifierName))
		output.emplace_back(move(highlight.location));
	return output;
}

void LanguageServer::findAllReferences(
	frontend::Declaration const* _declaration,
	string const& _sourceIdentifierName,
	frontend::SourceUnit const& _sourceUnit,
	std::vector<SourceLocation>& _output
)
{
	for (DocumentHighlight& highlight: ReferenceCollector::collect(_declaration, _sourceUnit, _sourceIdentifierName))
		_output.emplace_back(move(highlight.location));
}

vector<SourceLocation> LanguageServer::references(DocumentPosition _documentPosition)
{
	auto const file = m_fileReader->sourceCodes().find(_documentPosition.path);
	if (file == m_fileReader->sourceCodes().end())
		return {};

	if (!m_compilerStack)
		compile(_documentPosition.path);

	solAssert(m_compilerStack.get() != nullptr, "");

	string const& sourceName = _documentPosition.path;
	ASTNode const* sourceNode = findASTNode(_documentPosition.position, sourceName);
	if (!sourceNode)
		return {};

	frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
	vector<SourceLocation> output;
	matchASTNode(
		sourceNode,
		[&](Identifier const* sourceIdentifier)
		{
			if (auto decl = sourceIdentifier->annotation().referencedDeclaration)
				output += findAllReferences(decl, decl->name(), sourceUnit);
			for (auto const decl: sourceIdentifier->annotation().candidateDeclarations)
				output += findAllReferences(decl, decl->name(), sourceUnit);
		},
		[&](VariableDeclaration const* decl)
		{
			output += findAllReferences(decl, decl->name(), sourceUnit);
		},
		[&](FunctionDefinition const* functionDefinition)
		{
			output += findAllReferences(functionDefinition, functionDefinition->name(), sourceUnit);
		},
		[&](EnumDefinition const* enumDef)
		{
			output += findAllReferences(enumDef, enumDef->name(), sourceUnit);
		},
		[&](MemberAccess const* memberAccess)
		{
			if (Declaration const* decl = memberAccess->annotation().referencedDeclaration)
				output += findAllReferences(decl, memberAccess->memberName(), sourceUnit);
		},
		[&](ImportDirective const* importDirective)
		{
			output += findAllReferences(importDirective, importDirective->name(), sourceUnit);
		}
	);
	return output;
}

vector<DocumentHighlight> LanguageServer::semanticHighlight(DocumentPosition _documentPosition)
{
	if (!m_compilerStack || m_compilerStack->state() < frontend::CompilerStack::AnalysisPerformed)
		return {};

	frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(_documentPosition.path);
	ASTNode const* sourceNode = findASTNode(_documentPosition.position, _documentPosition.path);
	if (!sourceNode)
		return {};

	vector<DocumentHighlight> output;
	matchASTNode(
		sourceNode,
		[&](Identifier const* sourceIdentifier)
		{
			if (sourceIdentifier->annotation().referencedDeclaration)
				output += ReferenceCollector::collect(sourceIdentifier->annotation().referencedDeclaration, sourceUnit, sourceIdentifier->name());

			for (Declaration const* declaration: sourceIdentifier->annotation().candidateDeclarations)
				output += ReferenceCollector::collect(declaration, sourceUnit, sourceIdentifier->name());

			for (Declaration const* declaration: sourceIdentifier->annotation().overloadedDeclarations)
				output += ReferenceCollector::collect(declaration, sourceUnit, sourceIdentifier->name());
		},
		[&](VariableDeclaration const* varDecl)
		{
			output += ReferenceCollector::collect(varDecl, sourceUnit, varDecl->name());
		},
		[&](StructDefinition const* structDef)
		{
			output += ReferenceCollector::collect(structDef, sourceUnit, structDef->name());
		},
		[&](ContractDefinition const* contractDef)
		{
			output += ReferenceCollector::collect(contractDef, sourceUnit, contractDef->name());
		},
		[&](MemberAccess const* memberAccess)
		{
			Type const* type = memberAccess->expression().annotation().type;
			if (auto const ttype = dynamic_cast<TypeType const*>(type))
			{
				auto const memberName = memberAccess->memberName();

				if (auto const* enumType = dynamic_cast<EnumType const*>(ttype->actualType()))
				{
					// find the definition
					vector<DocumentHighlight> output;
					for (auto const& enumMember: enumType->enumDefinition().members())
						if (enumMember->name() == memberName)
							output += ReferenceCollector::collect(enumMember.get(), sourceUnit, enumMember->name());

					// TODO: find uses of the enum value
				}
			}
			else if (auto const structType = dynamic_cast<StructType const*>(type))
			{
				// TODO: highlight all struct member occurrences.
				// memberAccess->memberName()
				// structType->
			}
			else
			{
				// TODO: EnumType, ...
				trace("semanticHighlight: member type is: "s + (type ? typeid(*type).name() : "NULL"));
			}
		},
		[&](IdentifierPath const* identifierPath)
		{
			solAssert(!identifierPath->path().empty(), "");
			output += ReferenceCollector::collect(identifierPath->annotation().referencedDeclaration, sourceUnit, identifierPath->path().back());
		},
		[&](FunctionDefinition const* functionDefinition)
		{
			output += ReferenceCollector::collect(functionDefinition, sourceUnit, functionDefinition->name());
		},
		[&](EnumDefinition const* enumDef)
		{
			output += ReferenceCollector::collect(enumDef, sourceUnit, enumDef->name());
		},
		[&](ImportDirective const* importDirective)
		{
			output += ReferenceCollector::collect(importDirective, sourceUnit, importDirective->name());
		}
	);
	return output;
}

void LanguageServer::logNotImplemented(string_view _message)
{
	if (m_trace > Trace::Off && m_logger)
		m_logger(fmt::format("Not implemented. {}", _message));
}

bool LanguageServer::run()
{
	while (!m_exitRequested && !m_client->closed())
	{
		if (optional<Json::Value> const jsonMessage = m_client->receive(); jsonMessage.has_value())
		{
			try
			{
				handleMessage(jsonMessage.value());
			}
			catch (exception const& e)
			{
				log("Unhandled exception caught when handling message. "s + e.what());
			}
		}
	}
	return m_shutdownRequested;
}

void LanguageServer::handle_initialize(MessageID _id, Json::Value const& _args)
{
	string rootPath;
	if (Json::Value uri = _args["rootUri"])
		rootPath = extractPathFromFileURI(uri.asString()).value();
	else if (Json::Value rootPath = _args["rootPath"]; rootPath)
		rootPath = rootPath.asString();

	if (Json::Value value = _args["trace"]; value)
	{
		string const name = value.asString();
		if (name == "messages")
			m_trace = Trace::Messages;
		else if (name == "verbose")
			m_trace = Trace::Verbose;
		else if (name == "off")
			m_trace = Trace::Off;
	}

	m_basePath = boost::filesystem::path(rootPath);
	m_fileReader = make_unique<FileReader>(m_basePath, FileReader::FileSystemPathSet{m_basePath});
	if (_args["initializationOptions"].isObject())
		changeConfiguration(_args["initializationOptions"]);

	Json::Value replyArgs;
	replyArgs["serverInfo"]["name"] = "solc";
	replyArgs["serverInfo"]["version"] = string(solidity::frontend::VersionNumber);
	replyArgs["hoverProvider"] = true;
	replyArgs["capabilities"]["hoverProvider"] = true;
	replyArgs["capabilities"]["textDocumentSync"]["openClose"] = true;
	replyArgs["capabilities"]["textDocumentSync"]["change"] = 2; // 0=none, 1=full, 2=incremental
	replyArgs["capabilities"]["definitionProvider"] = true;
	replyArgs["capabilities"]["implementationProvider"] = true;
	replyArgs["capabilities"]["documentHighlightProvider"] = true;
	replyArgs["capabilities"]["referencesProvider"] = true;
	m_client->reply(_id, replyArgs);
}

void LanguageServer::handle_workspace_didChangeConfiguration(MessageID, Json::Value const& _args)
{
	if (_args["settings"].isObject())
		changeConfiguration(_args["settings"]);
}

void LanguageServer::handle_exit(MessageID _id, Json::Value const& /*_args*/)
{
	m_exitRequested = true;
	Json::Value replyArgs = Json::intValue;
	replyArgs = m_shutdownRequested ? 0 : 1;
	m_client->reply(_id, replyArgs);
}

void LanguageServer::handle_textDocument_didOpen(MessageID /*_id*/, Json::Value const& _args)
{
	if (!_args["textDocument"])
		return;

	auto const text = _args["textDocument"]["text"].asString();
	auto path = extractPathFromFileURI(_args["textDocument"]["uri"].asString()).value();
	if (boost::algorithm::starts_with(path, m_basePath.generic_string()))
		path = path.substr(m_basePath.generic_string().size());

	m_fileReader->setSource(path, text);
	compileSource(path);
}

void LanguageServer::handle_textDocument_didChange(MessageID /*_id*/, Json::Value const& _args)
{
	auto const path = extractPathFromFileURI(_args["textDocument"]["uri"].asString()).value();
	auto const contentChanges = _args["contentChanges"];

	for (Json::Value jsonContentChange: contentChanges)
	{
		if (!jsonContentChange.isObject()) // Protocol error, will only happen on broken clients, so silently ignore it.
			continue;

		string const text = jsonContentChange["text"].asString();

		if (jsonContentChange["range"].isObject())
		{
			Json::Value jsonRange = jsonContentChange["range"];
			LineColumnRange range{};
			range.start.line = jsonRange["start"]["line"].asInt();
			range.start.column = jsonRange["start"]["character"].asInt();
			range.end.line = jsonRange["end"]["line"].asInt();
			range.end.column = jsonRange["end"]["character"].asInt();
			documentContentUpdated(path, range, text);
		}
		else // full content update
			documentContentUpdated(path, move(text));
	}

	if (!contentChanges.empty())
		compileSource(path);
}

void LanguageServer::handleGotoDefAndImpl(MessageID _id, Json::Value const& _args)
{
	DocumentPosition const dpos = extractDocumentPosition(_args);

	solAssert(m_compilerStack.get() != nullptr, "source should be compiled already");

	if (!m_fileReader->sourceCodes().count(dpos.path))
	{
		Json::Value emptyResponse = Json::arrayValue;
		m_client->reply(_id, emptyResponse);
		return;
	}

	ASTNode const* sourceNode = findASTNode(dpos.position, dpos.path);
	if (!sourceNode)
	{
		Json::Value emptyResponse = Json::arrayValue;
		m_client->reply(_id, emptyResponse);
		return;
	}

	vector<SourceLocation> locations;
	matchASTNode(
		sourceNode,
		[&](ImportDirective const* importDirective)
		{
			auto const& path = *importDirective->annotation().absolutePath;
			auto const i = m_fileReader->sourceCodes().find(path);
			if (i != m_fileReader->sourceCodes().end())
				locations.emplace_back(SourceLocation{0, 0, make_shared<CharStream>("", path)});
		},
		[&](MemberAccess const* memberAccess) // For scope members, jump to the naming symbol of the referencing declaration of this member.
		{
			auto const declaration = memberAccess->annotation().referencedDeclaration;
			auto const location = declarationPosition(declaration);
			if (location.has_value())
				locations.emplace_back(location.value());
		},
		[&](Identifier const* sourceIdentifier) // For identifiers, jump to the naming symbol of the definition of this identifier.
		{
			if (Declaration const* decl = sourceIdentifier->annotation().referencedDeclaration)
				if (auto location = declarationPosition(decl); location.has_value())
					locations.emplace_back(move(location.value()));
			for (auto const declaration: sourceIdentifier->annotation().candidateDeclarations)
				if (auto location = declarationPosition(declaration); location.has_value())
					locations.emplace_back(move(location.value()));
		}
	);

	Json::Value reply = Json::arrayValue;
	for (SourceLocation const& location: locations)
		reply.append(toJson(m_basePath, location));
	m_client->reply(_id, reply);
}

string LanguageServer::symbolHoverInformation(frontend::ASTNode const* _sourceNode)
{
	if (auto const* documented = dynamic_cast<StructurallyDocumented const*>(_sourceNode))
	{
		if (documented->documentation())
			return *documented->documentation()->text();
	}

	string tooltipText{};
	matchASTNode(
		_sourceNode,
		[&](Identifier const* identifier)
		{
			Type const* ty = identifier->annotation().type;
			tooltipText += ty->toString(false);
		},
		[&](MemberAccess const* memberAccess)
		{
			if (memberAccess->annotation().type)
				tooltipText += memberAccess->annotation().type->toString(false);
		},
		[&](IdentifierPath const* identifierPath)
		{
			Declaration const* decl = identifierPath->annotation().referencedDeclaration;
			if (decl && decl->type())
				tooltipText += decl->type()->toString(false);
		}
	);

	return tooltipText;
}


void LanguageServer::handle_textDocument_hover(MessageID _id, Json::Value const& _args)
{
	auto const dpos = extractDocumentPosition(_args);

	auto const sourceNode = findASTNode(dpos.position, dpos.path);
	if (!sourceNode)
	{
		Json::Value emptyResponse = Json::arrayValue;
		m_client->reply(_id, emptyResponse); // reply with "No references".
		return;
	}

	string tooltipText = symbolHoverInformation(sourceNode);
	if (tooltipText.empty())
		return;

	Json::Value reply = Json::objectValue;
	reply["range"] = toJsonRange(sourceNode->location());
	reply["contents"]["kind"] = "markdown";
	reply["contents"]["value"] = move(tooltipText);

	m_client->reply(_id, reply);
}

void LanguageServer::handle_textDocument_highlight(MessageID _id, Json::Value const& _args)
{
	auto const dpos = extractDocumentPosition(_args);

	if (!m_compilerStack)
		compile(dpos.path);

	Json::Value jsonReply = Json::arrayValue;
	for (DocumentHighlight const& highlight: semanticHighlight(dpos))
	{
		Json::Value item = Json::objectValue;
		item["range"] = toJsonRange(highlight.location);
		if (highlight.kind != DocumentHighlightKind::Unspecified)
			item["kind"] = static_cast<int>(highlight.kind);

		jsonReply.append(item);
	}
	m_client->reply(_id, jsonReply);
}

void LanguageServer::handle_textDocument_references(MessageID _id, Json::Value const& _args)
{
	auto const dpos = extractDocumentPosition(_args);

	auto file = m_fileReader->sourceCodes().find(dpos.path);
	if (file == m_fileReader->sourceCodes().end())
	{
		Json::Value emptyResponse = Json::arrayValue;
		m_client->reply(_id, emptyResponse); // reply with "No references".
		return;
	}

	if (!m_compilerStack)
		compile(dpos.path);

	solAssert(m_compilerStack.get() != nullptr, "");

	auto const sourceNode = findASTNode(dpos.position, dpos.path);
	if (!sourceNode)
	{
		Json::Value emptyResponse = Json::arrayValue;
		m_client->reply(_id, emptyResponse); // reply with "No references".
		return;
	}

	auto locations = vector<SourceLocation>{};
	matchASTNode(
		sourceNode,
		[&](Identifier const* sourceIdentifier)
		{
			frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(dpos.path);

			if (auto decl = sourceIdentifier->annotation().referencedDeclaration)
				findAllReferences(decl, decl->name(), sourceUnit, locations);

			for (auto const decl: sourceIdentifier->annotation().candidateDeclarations)
				findAllReferences(decl, decl->name(), sourceUnit, locations);

			for (auto const decl: sourceIdentifier->annotation().overloadedDeclarations)
				findAllReferences(decl, decl->name(), sourceUnit, locations);
		},
		[&](VariableDeclaration const* decl)
		{
			frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(dpos.path);
			findAllReferences(decl, decl->name(), sourceUnit, locations);
		},
		[&](FunctionDefinition const* functionDefinition)
		{
			frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(dpos.path);
			findAllReferences(functionDefinition, functionDefinition->name(), sourceUnit, locations);
		},
		[&](EnumDefinition const* enumDef)
		{
			frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(dpos.path);
			findAllReferences(enumDef, enumDef->name(), sourceUnit, locations);
		},
		[&](MemberAccess const* memberAccess)
		{
			if (Declaration const* decl = memberAccess->annotation().referencedDeclaration)
			{
				frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(dpos.path);
				findAllReferences(decl, memberAccess->memberName(), sourceUnit, locations);
			}
		}
	);

	Json::Value jsonReply = Json::arrayValue;
	for (SourceLocation const& location: locations)
		jsonReply.append(toJson(m_basePath, location));

	m_client->reply(_id, jsonReply);
}

void LanguageServer::log(string _message)
{
	if (m_trace >= Trace::Messages && m_logger)
		m_logger(_message);
}

void LanguageServer::trace(string const& _message)
{
	if (m_trace >= Trace::Verbose && m_logger)
		m_logger(_message);
}

void LanguageServer::handleMessage(Json::Value const& _jsonMessage)
{
	string const methodName = _jsonMessage["method"].asString();

	MessageID const id = _jsonMessage["id"].isInt()
		? MessageID{to_string(_jsonMessage["id"].asInt())}
		: _jsonMessage["id"].isString()
			? MessageID{_jsonMessage["id"].asString()}
			: MessageID{};

	auto const handler = m_handlers.find(methodName);
	if (handler == m_handlers.end())
		m_client->error(id, ErrorCode::MethodNotFound, "Unknown method " + methodName);
	else if (handler->second)
	{
		Json::Value const& jsonArgs = _jsonMessage["params"];
		handler->second(id, jsonArgs);
	}
}

} // namespace solidity
