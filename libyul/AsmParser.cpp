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
/**
 * @author Christian <c@ethdev.com>
 * @date 2016
 * Solidity inline assembly parser.
 */

#include <libyul/AsmParser.h>
#include <libyul/Exceptions.h>
#include <liblangutil/Scanner.h>
#include <liblangutil/ErrorReporter.h>
#include <libsolutil/Common.h>

#include <boost/algorithm/string.hpp>

#include <cctype>
#include <algorithm>

using namespace std;
using namespace solidity;
using namespace solidity::util;
using namespace solidity::langutil;
using namespace solidity::yul;

shared_ptr<Block> Parser::parse(std::shared_ptr<Scanner> const& _scanner, bool _reuseScanner)
{
	m_recursionDepth = 0;

	_scanner->supportPeriodInIdentifier(true);
	ScopeGuard resetScanner([&]{ _scanner->supportPeriodInIdentifier(false); });

	try
	{
		m_scanner = _scanner;
		auto block = make_shared<Block>(parseBlock());
		if (!_reuseScanner)
			expectToken(Token::EOS);
		return block;
	}
	catch (FatalError const&)
	{
		yulAssert(!m_errorReporter.errors().empty(), "Fatal error detected, but no error is reported.");
	}

	return nullptr;
}

std::map<string, evmasm::Instruction> const& Parser::instructions()
{
	// Allowed instructions, lowercase names.
	static map<string, evmasm::Instruction> s_instructions;
	if (s_instructions.empty())
	{
		for (auto const& instruction: evmasm::c_instructions)
		{
			if (
				instruction.second == evmasm::Instruction::JUMPDEST ||
				evmasm::isPushInstruction(instruction.second)
			)
				continue;
			string name = instruction.first;
			transform(name.begin(), name.end(), name.begin(), [](unsigned char _c) { return tolower(_c); });
			s_instructions[name] = instruction.second;
		}
	}
	return s_instructions;
}

Block Parser::parseBlock()
{
	RecursionGuard recursionGuard(*this);
	Block block = createWithLocation<Block>();
	expectToken(Token::LBrace);
	while (currentToken() != Token::RBrace)
		block.statements.emplace_back(parseStatement());
	block.location.end = endPosition();
	advance();
	return block;
}

Statement Parser::parseStatement()
{
	RecursionGuard recursionGuard(*this);
	switch (currentToken())
	{
	case Token::Let:
		return parseVariableDeclaration();
	case Token::Function:
		return parseFunctionDefinition();
	case Token::LBrace:
		return parseBlock();
	case Token::If:
	{
		If _if = createWithLocation<If>();
		advance();
		_if.condition = make_unique<Expression>(parseExpression());
		_if.body = parseBlock();
		return Statement{move(_if)};
	}
	case Token::Switch:
	{
		Switch _switch = createWithLocation<Switch>();
		advance();
		_switch.expression = make_unique<Expression>(parseExpression());
		while (currentToken() == Token::Case)
			_switch.cases.emplace_back(parseCase());
		if (currentToken() == Token::Default)
			_switch.cases.emplace_back(parseCase());
		if (currentToken() == Token::Default)
			fatalParserError("Only one default case allowed.");
		else if (currentToken() == Token::Case)
			fatalParserError("Case not allowed after default case.");
		if (_switch.cases.empty())
			fatalParserError("Switch statement without any cases.");
		_switch.location.end = _switch.cases.back().body.location.end;
		return Statement{move(_switch)};
	}
	case Token::For:
		return parseForLoop();
	case Token::Break:
	{
		Statement stmt{createWithLocation<Break>()};
		checkBreakContinuePosition("break");
		m_scanner->next();
		return stmt;
	}
	case Token::Continue:
	{
		Statement stmt{createWithLocation<Continue>()};
		checkBreakContinuePosition("continue");
		m_scanner->next();
		return stmt;
	}
	case Token::Identifier:
		if (currentLiteral() == "leave")
		{
			Statement stmt{createWithLocation<Leave>()};
			if (!m_insideFunction)
				m_errorReporter.syntaxError(location(), "Keyword \"leave\" can only be used inside a function.");
			m_scanner->next();
			return stmt;
		}
		break;
	default:
		break;
	}
	// Options left:
	// Simple instruction (might turn into functional),
	// literal,
	// identifier (might turn into label or functional assignment)
	ElementaryOperation elementary(parseElementaryOperation());

	switch (currentToken())
	{
	case Token::LParen:
	{
		Expression expr = parseCall(std::move(elementary));
		return ExpressionStatement{locationOf(expr), expr};
	}
	case Token::Comma:
	case Token::AssemblyAssign:
	{
		std::vector<Identifier> variableNames;

		while (true)
		{
			if (!holds_alternative<Identifier>(elementary))
			{
				auto const token = currentToken() == Token::Comma ? "," : ":=";

				fatalParserError(
					std::string("Variable name must precede \"") +
					token +
					"\"" +
					(currentToken() == Token::Comma ? " in multiple assignment." : " in assignment.")
				);
			}

			auto const& identifier = std::get<Identifier>(elementary);

			if (m_dialect.builtin(identifier.name))
				fatalParserError("Cannot assign to builtin function \"" + identifier.name.str() + "\".");

			variableNames.emplace_back(identifier);

			if (currentToken() != Token::Comma)
				break;

			expectToken(Token::Comma);

			elementary = parseElementaryOperation();
		}

		Assignment assignment =
			createWithLocation<Assignment>(std::get<Identifier>(elementary).location);
		assignment.variableNames = std::move(variableNames);

		expectToken(Token::AssemblyAssign);

		assignment.value = make_unique<Expression>(parseExpression());
		assignment.location.end = locationOf(*assignment.value).end;

		return Statement{std::move(assignment)};
	}
	default:
		fatalParserError("Call or assignment expected.");
		break;
	}

	if (holds_alternative<Identifier>(elementary))
	{
		Identifier& identifier = std::get<Identifier>(elementary);
		return ExpressionStatement{identifier.location, { move(identifier) }};
	}
	else if (holds_alternative<Literal>(elementary))
	{
		Expression expr = std::get<Literal>(elementary);
		return ExpressionStatement{locationOf(expr), expr};
	}
	else
	{
		yulAssert(false, "Invalid elementary operation.");
		return {};
	}
}

Case Parser::parseCase()
{
	RecursionGuard recursionGuard(*this);
	Case _case = createWithLocation<Case>();
	if (currentToken() == Token::Default)
		advance();
	else if (currentToken() == Token::Case)
	{
		advance();
		ElementaryOperation literal = parseElementaryOperation();
		if (!holds_alternative<Literal>(literal))
			fatalParserError("Literal expected.");
		_case.value = make_unique<Literal>(std::get<Literal>(std::move(literal)));
	}
	else
		yulAssert(false, "Case or default case expected.");
	_case.body = parseBlock();
	_case.location.end = _case.body.location.end;
	return _case;
}

ForLoop Parser::parseForLoop()
{
	RecursionGuard recursionGuard(*this);

	ForLoopComponent outerForLoopComponent = m_currentForLoopComponent;

	ForLoop forLoop = createWithLocation<ForLoop>();
	expectToken(Token::For);
	m_currentForLoopComponent = ForLoopComponent::ForLoopPre;
	forLoop.pre = parseBlock();
	m_currentForLoopComponent = ForLoopComponent::None;
	forLoop.condition = make_unique<Expression>(parseExpression());
	m_currentForLoopComponent = ForLoopComponent::ForLoopPost;
	forLoop.post = parseBlock();
	m_currentForLoopComponent = ForLoopComponent::ForLoopBody;
	forLoop.body = parseBlock();
	forLoop.location.end = forLoop.body.location.end;

	m_currentForLoopComponent = outerForLoopComponent;

	return forLoop;
}

Expression Parser::parseExpression()
{
	RecursionGuard recursionGuard(*this);

	ElementaryOperation operation = parseElementaryOperation();
	if (holds_alternative<FunctionCall>(operation) || currentToken() == Token::LParen)
		return parseCall(std::move(operation));
	else if (holds_alternative<Identifier>(operation))
		return std::get<Identifier>(operation);
	else
	{
		yulAssert(holds_alternative<Literal>(operation), "");
		return std::get<Literal>(operation);
	}
}

std::map<evmasm::Instruction, string> const& Parser::instructionNames()
{
	static map<evmasm::Instruction, string> s_instructionNames;
	if (s_instructionNames.empty())
	{
		for (auto const& instr: instructions())
			s_instructionNames[instr.second] = instr.first;
		// set the ambiguous instructions to a clear default
		s_instructionNames[evmasm::Instruction::SELFDESTRUCT] = "selfdestruct";
		s_instructionNames[evmasm::Instruction::KECCAK256] = "keccak256";
	}
	return s_instructionNames;
}

Parser::ElementaryOperation Parser::parseElementaryOperation()
{
	RecursionGuard recursionGuard(*this);
	ElementaryOperation ret;
	switch (currentToken())
	{
	case Token::Identifier:
	case Token::Return:
	case Token::Byte:
	case Token::Bool:
	case Token::Address:
	{
		YulString literal{currentLiteral()};
		if (m_dialect.builtin(literal))
		{
			Identifier identifier{location(), literal};
			advance();
			expectToken(Token::LParen, false);
			return FunctionCall{identifier.location, identifier, {}};
		}
		else
			ret = Identifier{location(), literal};
		advance();
		break;
	}
	case Token::StringLiteral:
	case Token::Number:
	case Token::TrueLiteral:
	case Token::FalseLiteral:
	{
		LiteralKind kind = LiteralKind::Number;
		switch (currentToken())
		{
		case Token::StringLiteral:
			kind = LiteralKind::String;
			break;
		case Token::Number:
			if (!isValidNumberLiteral(currentLiteral()))
				fatalParserError("Invalid number literal.");
			kind = LiteralKind::Number;
			break;
		case Token::TrueLiteral:
		case Token::FalseLiteral:
			kind = LiteralKind::Boolean;
			break;
		default:
			break;
		}

		Literal literal{
			location(),
			kind,
			YulString{currentLiteral()},
			{}
		};
		advance();
		if (m_dialect.flavour == AsmFlavour::Yul)
		{
			expectToken(Token::Colon);
			literal.location.end = endPosition();
			literal.type = expectAsmIdentifier();
		}
		else if (kind == LiteralKind::Boolean)
			fatalParserError("True and false are not valid literals.");
		ret = std::move(literal);
		break;
	}
	default:
		fatalParserError(
			m_dialect.flavour == AsmFlavour::Yul ?
			"Literal or identifier expected." :
			"Literal, identifier or instruction expected."
		);
	}
	return ret;
}

VariableDeclaration Parser::parseVariableDeclaration()
{
	RecursionGuard recursionGuard(*this);
	VariableDeclaration varDecl = createWithLocation<VariableDeclaration>();
	expectToken(Token::Let);
	while (true)
	{
		varDecl.variables.emplace_back(parseTypedName());
		if (currentToken() == Token::Comma)
			expectToken(Token::Comma);
		else
			break;
	}
	if (currentToken() == Token::AssemblyAssign)
	{
		expectToken(Token::AssemblyAssign);
		varDecl.value = make_unique<Expression>(parseExpression());
		varDecl.location.end = locationOf(*varDecl.value).end;
	}
	else
		varDecl.location.end = varDecl.variables.back().location.end;
	return varDecl;
}

FunctionDefinition Parser::parseFunctionDefinition()
{
	RecursionGuard recursionGuard(*this);

	if (m_currentForLoopComponent == ForLoopComponent::ForLoopPre)
		m_errorReporter.syntaxError(
			location(),
			"Functions cannot be defined inside a for-loop init block."
		);

	ForLoopComponent outerForLoopComponent = m_currentForLoopComponent;
	m_currentForLoopComponent = ForLoopComponent::None;

	FunctionDefinition funDef = createWithLocation<FunctionDefinition>();
	expectToken(Token::Function);
	funDef.name = expectAsmIdentifier();
	expectToken(Token::LParen);
	while (currentToken() != Token::RParen)
	{
		funDef.parameters.emplace_back(parseTypedName());
		if (currentToken() == Token::RParen)
			break;
		expectToken(Token::Comma);
	}
	expectToken(Token::RParen);
	if (currentToken() == Token::Sub)
	{
		expectToken(Token::Sub);
		expectToken(Token::GreaterThan);
		while (true)
		{
			funDef.returnVariables.emplace_back(parseTypedName());
			if (currentToken() == Token::LBrace)
				break;
			expectToken(Token::Comma);
		}
	}
	bool preInsideFunction = m_insideFunction;
	m_insideFunction = true;
	funDef.body = parseBlock();
	m_insideFunction = preInsideFunction;
	funDef.location.end = funDef.body.location.end;

	m_currentForLoopComponent = outerForLoopComponent;
	return funDef;
}

Expression Parser::parseCall(Parser::ElementaryOperation&& _initialOp)
{
	RecursionGuard recursionGuard(*this);

	FunctionCall ret;
	if (holds_alternative<Identifier>(_initialOp))
	{
		ret.functionName = std::move(std::get<Identifier>(_initialOp));
		ret.location = ret.functionName.location;
	}
	else if (holds_alternative<FunctionCall>(_initialOp))
		ret = std::move(std::get<FunctionCall>(_initialOp));
	else
		fatalParserError(
			m_dialect.flavour == AsmFlavour::Yul ?
			"Function name expected." :
			"Assembly instruction or function name required in front of \"(\")"
		);

	expectToken(Token::LParen);
	if (currentToken() != Token::RParen)
	{
		ret.arguments.emplace_back(parseExpression());
		while (currentToken() != Token::RParen)
		{
			expectToken(Token::Comma);
			ret.arguments.emplace_back(parseExpression());
		}
	}
	ret.location.end = endPosition();
	expectToken(Token::RParen);
	return ret;
}

TypedName Parser::parseTypedName()
{
	RecursionGuard recursionGuard(*this);
	TypedName typedName = createWithLocation<TypedName>();
	typedName.name = expectAsmIdentifier();
	if (m_dialect.flavour == AsmFlavour::Yul)
	{
		expectToken(Token::Colon);
		typedName.location.end = endPosition();
		typedName.type = expectAsmIdentifier();
	}
	return typedName;
}

YulString Parser::expectAsmIdentifier()
{
	YulString name{currentLiteral()};
	switch (currentToken())
	{
	case Token::Return:
	case Token::Byte:
	case Token::Address:
	case Token::Bool:
	case Token::Identifier:
		break;
	default:
		expectToken(Token::Identifier);
		break;
	}

	if (m_dialect.builtin(name))
		fatalParserError("Cannot use builtin function name \"" + name.str() + "\" as identifier name.");
	advance();
	return name;
}

void Parser::checkBreakContinuePosition(string const& _which)
{
	switch (m_currentForLoopComponent)
	{
	case ForLoopComponent::None:
		m_errorReporter.syntaxError(location(), "Keyword \"" + _which + "\" needs to be inside a for-loop body.");
		break;
	case ForLoopComponent::ForLoopPre:
		m_errorReporter.syntaxError(location(), "Keyword \"" + _which + "\" in for-loop init block is not allowed.");
		break;
	case ForLoopComponent::ForLoopPost:
		m_errorReporter.syntaxError(location(), "Keyword \"" + _which + "\" in for-loop post block is not allowed.");
		break;
	case ForLoopComponent::ForLoopBody:
		break;
	}
}

bool Parser::isValidNumberLiteral(string const& _literal)
{
	try
	{
		// Try to convert _literal to u256.
		[[maybe_unused]] auto tmp = u256(_literal);
	}
	catch (...)
	{
		return false;
	}
	if (boost::starts_with(_literal, "0x"))
		return true;
	else
		return _literal.find_first_not_of("0123456789") == string::npos;
}
