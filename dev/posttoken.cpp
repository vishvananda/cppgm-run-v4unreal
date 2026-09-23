// (C) 2013 CPPGM Foundation www.cppgm.org.  All rights reserved.

#include <iostream>
#include <sstream>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cassert>
#include <memory>
#include <cstring>
#include <cstdint>
#include <climits>
#include <map>
#include <vector>
#include <limits>
#include <cstdlib>

#include "preprocess/tokens/IPPTokenStream.h"
#include "preprocess/tokens/PPTokenizer.h"

using namespace std;

#include "support/not_implemented.h"

// See 3.9.1: Fundamental Types
enum EFundamentalType
{
	// 3.9.1.2
	FT_SIGNED_CHAR,
	FT_SHORT_INT,
	FT_INT,
	FT_LONG_INT,
	FT_LONG_LONG_INT,

	// 3.9.1.3
	FT_UNSIGNED_CHAR,
	FT_UNSIGNED_SHORT_INT,
	FT_UNSIGNED_INT,
	FT_UNSIGNED_LONG_INT,
	FT_UNSIGNED_LONG_LONG_INT,

	// 3.9.1.1 / 3.9.1.5
	FT_WCHAR_T,
	FT_CHAR,
	FT_CHAR16_T,
	FT_CHAR32_T,

	// 3.9.1.6
	FT_BOOL,

	// 3.9.1.8
	FT_FLOAT,
	FT_DOUBLE,
	FT_LONG_DOUBLE,

	// 3.9.1.9
	FT_VOID,

	// 3.9.1.10
	FT_NULLPTR_T
};

// FundamentalTypeOf: convert fundamental type T to EFundamentalType
// for example: `FundamentalTypeOf<long int>()` will return `FT_LONG_INT`
template<typename T> constexpr EFundamentalType FundamentalTypeOf();
template<> constexpr EFundamentalType FundamentalTypeOf<signed char>() { return FT_SIGNED_CHAR; }
template<> constexpr EFundamentalType FundamentalTypeOf<short int>() { return FT_SHORT_INT; }
template<> constexpr EFundamentalType FundamentalTypeOf<int>() { return FT_INT; }
template<> constexpr EFundamentalType FundamentalTypeOf<long int>() { return FT_LONG_INT; }
template<> constexpr EFundamentalType FundamentalTypeOf<long long int>() { return FT_LONG_LONG_INT; }
template<> constexpr EFundamentalType FundamentalTypeOf<unsigned char>() { return FT_UNSIGNED_CHAR; }
template<> constexpr EFundamentalType FundamentalTypeOf<unsigned short int>() { return FT_UNSIGNED_SHORT_INT; }
template<> constexpr EFundamentalType FundamentalTypeOf<unsigned int>() { return FT_UNSIGNED_INT; }
template<> constexpr EFundamentalType FundamentalTypeOf<unsigned long int>() { return FT_UNSIGNED_LONG_INT; }
template<> constexpr EFundamentalType FundamentalTypeOf<unsigned long long int>() { return FT_UNSIGNED_LONG_LONG_INT; }
template<> constexpr EFundamentalType FundamentalTypeOf<wchar_t>() { return FT_WCHAR_T; }
template<> constexpr EFundamentalType FundamentalTypeOf<char>() { return FT_CHAR; }
template<> constexpr EFundamentalType FundamentalTypeOf<char16_t>() { return FT_CHAR16_T; }
template<> constexpr EFundamentalType FundamentalTypeOf<char32_t>() { return FT_CHAR32_T; }
template<> constexpr EFundamentalType FundamentalTypeOf<bool>() { return FT_BOOL; }
template<> constexpr EFundamentalType FundamentalTypeOf<float>() { return FT_FLOAT; }
template<> constexpr EFundamentalType FundamentalTypeOf<double>() { return FT_DOUBLE; }
template<> constexpr EFundamentalType FundamentalTypeOf<long double>() { return FT_LONG_DOUBLE; }
template<> constexpr EFundamentalType FundamentalTypeOf<void>() { return FT_VOID; }
template<> constexpr EFundamentalType FundamentalTypeOf<nullptr_t>() { return FT_NULLPTR_T; }

// convert EFundamentalType to a source code
const map<EFundamentalType, string> FundamentalTypeToStringMap
{
	{FT_SIGNED_CHAR, "signed char"},
	{FT_SHORT_INT, "short int"},
	{FT_INT, "int"},
	{FT_LONG_INT, "long int"},
	{FT_LONG_LONG_INT, "long long int"},
	{FT_UNSIGNED_CHAR, "unsigned char"},
	{FT_UNSIGNED_SHORT_INT, "unsigned short int"},
	{FT_UNSIGNED_INT, "unsigned int"},
	{FT_UNSIGNED_LONG_INT, "unsigned long int"},
	{FT_UNSIGNED_LONG_LONG_INT, "unsigned long long int"},
	{FT_WCHAR_T, "wchar_t"},
	{FT_CHAR, "char"},
	{FT_CHAR16_T, "char16_t"},
	{FT_CHAR32_T, "char32_t"},
	{FT_BOOL, "bool"},
	{FT_FLOAT, "float"},
	{FT_DOUBLE, "double"},
	{FT_LONG_DOUBLE, "long double"},
	{FT_VOID, "void"},
	{FT_NULLPTR_T, "nullptr_t"}
};

// token type enum for `simples`
enum ETokenType
{
	// keywords
	KW_ALIGNAS,
	KW_ALIGNOF,
	KW_ASM,
	KW_AUTO,
	KW_BOOL,
	KW_BREAK,
	KW_CASE,
	KW_CATCH,
	KW_CHAR,
	KW_CHAR16_T,
	KW_CHAR32_T,
	KW_CLASS,
	KW_CONST,
	KW_CONSTEXPR,
	KW_CONST_CAST,
	KW_CONTINUE,
	KW_DECLTYPE,
	KW_DEFAULT,
	KW_DELETE,
	KW_DO,
	KW_DOUBLE,
	KW_DYNAMIC_CAST,
	KW_ELSE,
	KW_ENUM,
	KW_EXPLICIT,
	KW_EXPORT,
	KW_EXTERN,
	KW_FALSE,
	KW_FLOAT,
	KW_FOR,
	KW_FRIEND,
	KW_GOTO,
	KW_IF,
	KW_INLINE,
	KW_INT,
	KW_LONG,
	KW_MUTABLE,
	KW_NAMESPACE,
	KW_NEW,
	KW_NOEXCEPT,
	KW_NULLPTR,
	KW_OPERATOR,
	KW_PRIVATE,
	KW_PROTECTED,
	KW_PUBLIC,
	KW_REGISTER,
	KW_REINTERPET_CAST,
	KW_RETURN,
	KW_SHORT,
	KW_SIGNED,
	KW_SIZEOF,
	KW_STATIC,
	KW_STATIC_ASSERT,
	KW_STATIC_CAST,
	KW_STRUCT,
	KW_SWITCH,
	KW_TEMPLATE,
	KW_THIS,
	KW_THREAD_LOCAL,
	KW_THROW,
	KW_TRUE,
	KW_TRY,
	KW_TYPEDEF,
	KW_TYPEID,
	KW_TYPENAME,
	KW_UNION,
	KW_UNSIGNED,
	KW_USING,
	KW_VIRTUAL,
	KW_VOID,
	KW_VOLATILE,
	KW_WCHAR_T,
	KW_WHILE,

	// operators/punctuation
	OP_LBRACE,
	OP_RBRACE,
	OP_LSQUARE,
	OP_RSQUARE,
	OP_LPAREN,
	OP_RPAREN,
	OP_BOR,
	OP_XOR,
	OP_COMPL,
	OP_AMP,
	OP_LNOT,
	OP_SEMICOLON,
	OP_COLON,
	OP_DOTS,
	OP_QMARK,
	OP_COLON2,
	OP_DOT,
	OP_DOTSTAR,
	OP_PLUS,
	OP_MINUS,
	OP_STAR,
	OP_DIV,
	OP_MOD,
	OP_ASS,
	OP_LT,
	OP_GT,
	OP_PLUSASS,
	OP_MINUSASS,
	OP_STARASS,
	OP_DIVASS,
	OP_MODASS,
	OP_XORASS,
	OP_BANDASS,
	OP_BORASS,
	OP_LSHIFT,
	OP_RSHIFT,
	OP_RSHIFTASS,
	OP_LSHIFTASS,
	OP_EQ,
	OP_NE,
	OP_LE,
	OP_GE,
	OP_LAND,
	OP_LOR,
	OP_INC,
	OP_DEC,
	OP_COMMA,
	OP_ARROWSTAR,
	OP_ARROW,
};

// StringToETokenTypeMap map of `simple` `preprocessing-tokens` to ETokenType
const unordered_map<string, ETokenType> StringToTokenTypeMap =
{
	// keywords
	{"alignas", KW_ALIGNAS},
	{"alignof", KW_ALIGNOF},
	{"asm", KW_ASM},
	{"auto", KW_AUTO},
	{"bool", KW_BOOL},
	{"break", KW_BREAK},
	{"case", KW_CASE},
	{"catch", KW_CATCH},
	{"char", KW_CHAR},
	{"char16_t", KW_CHAR16_T},
	{"char32_t", KW_CHAR32_T},
	{"class", KW_CLASS},
	{"const", KW_CONST},
	{"constexpr", KW_CONSTEXPR},
	{"const_cast", KW_CONST_CAST},
	{"continue", KW_CONTINUE},
	{"decltype", KW_DECLTYPE},
	{"default", KW_DEFAULT},
	{"delete", KW_DELETE},
	{"do", KW_DO},
	{"double", KW_DOUBLE},
	{"dynamic_cast", KW_DYNAMIC_CAST},
	{"else", KW_ELSE},
	{"enum", KW_ENUM},
	{"explicit", KW_EXPLICIT},
	{"export", KW_EXPORT},
	{"extern", KW_EXTERN},
	{"false", KW_FALSE},
	{"float", KW_FLOAT},
	{"for", KW_FOR},
	{"friend", KW_FRIEND},
	{"goto", KW_GOTO},
	{"if", KW_IF},
	{"inline", KW_INLINE},
	{"int", KW_INT},
	{"long", KW_LONG},
	{"mutable", KW_MUTABLE},
	{"namespace", KW_NAMESPACE},
	{"new", KW_NEW},
	{"noexcept", KW_NOEXCEPT},
	{"nullptr", KW_NULLPTR},
	{"operator", KW_OPERATOR},
	{"private", KW_PRIVATE},
	{"protected", KW_PROTECTED},
	{"public", KW_PUBLIC},
	{"register", KW_REGISTER},
	{"reinterpret_cast", KW_REINTERPET_CAST},
	{"return", KW_RETURN},
	{"short", KW_SHORT},
	{"signed", KW_SIGNED},
	{"sizeof", KW_SIZEOF},
	{"static", KW_STATIC},
	{"static_assert", KW_STATIC_ASSERT},
	{"static_cast", KW_STATIC_CAST},
	{"struct", KW_STRUCT},
	{"switch", KW_SWITCH},
	{"template", KW_TEMPLATE},
	{"this", KW_THIS},
	{"thread_local", KW_THREAD_LOCAL},
	{"throw", KW_THROW},
	{"true", KW_TRUE},
	{"try", KW_TRY},
	{"typedef", KW_TYPEDEF},
	{"typeid", KW_TYPEID},
	{"typename", KW_TYPENAME},
	{"union", KW_UNION},
	{"unsigned", KW_UNSIGNED},
	{"using", KW_USING},
	{"virtual", KW_VIRTUAL},
	{"void", KW_VOID},
	{"volatile", KW_VOLATILE},
	{"wchar_t", KW_WCHAR_T},
	{"while", KW_WHILE},

	// operators/punctuation
	{"{", OP_LBRACE},
	{"<%", OP_LBRACE},
	{"}", OP_RBRACE},
	{"%>", OP_RBRACE},
	{"[", OP_LSQUARE},
	{"<:", OP_LSQUARE},
	{"]", OP_RSQUARE},
	{":>", OP_RSQUARE},
	{"(", OP_LPAREN},
	{")", OP_RPAREN},
	{"|", OP_BOR},
	{"bitor", OP_BOR},
	{"^", OP_XOR},
	{"xor", OP_XOR},
	{"~", OP_COMPL},
	{"compl", OP_COMPL},
	{"&", OP_AMP},
	{"bitand", OP_AMP},
	{"!", OP_LNOT},
	{"not", OP_LNOT},
	{";", OP_SEMICOLON},
	{":", OP_COLON},
	{"...", OP_DOTS},
	{"?", OP_QMARK},
	{"::", OP_COLON2},
	{".", OP_DOT},
	{".*", OP_DOTSTAR},
	{"+", OP_PLUS},
	{"-", OP_MINUS},
	{"*", OP_STAR},
	{"/", OP_DIV},
	{"%", OP_MOD},
	{"=", OP_ASS},
	{"<", OP_LT},
	{">", OP_GT},
	{"+=", OP_PLUSASS},
	{"-=", OP_MINUSASS},
	{"*=", OP_STARASS},
	{"/=", OP_DIVASS},
	{"%=", OP_MODASS},
	{"^=", OP_XORASS},
	{"xor_eq", OP_XORASS},
	{"&=", OP_BANDASS},
	{"and_eq", OP_BANDASS},
	{"|=", OP_BORASS},
	{"or_eq", OP_BORASS},
	{"<<", OP_LSHIFT},
	{">>", OP_RSHIFT},
	{">>=", OP_RSHIFTASS},
	{"<<=", OP_LSHIFTASS},
	{"==", OP_EQ},
	{"!=", OP_NE},
	{"not_eq", OP_NE},
	{"<=", OP_LE},
	{">=", OP_GE},
	{"&&", OP_LAND},
	{"and", OP_LAND},
	{"||", OP_LOR},
	{"or", OP_LOR},
	{"++", OP_INC},
	{"--", OP_DEC},
	{",", OP_COMMA},
	{"->*", OP_ARROWSTAR},
	{"->", OP_ARROW}
};

// map of enum to string
const map<ETokenType, string> TokenTypeToStringMap =
{
	{KW_ALIGNAS, "KW_ALIGNAS"},
	{KW_ALIGNOF, "KW_ALIGNOF"},
	{KW_ASM, "KW_ASM"},
	{KW_AUTO, "KW_AUTO"},
	{KW_BOOL, "KW_BOOL"},
	{KW_BREAK, "KW_BREAK"},
	{KW_CASE, "KW_CASE"},
	{KW_CATCH, "KW_CATCH"},
	{KW_CHAR, "KW_CHAR"},
	{KW_CHAR16_T, "KW_CHAR16_T"},
	{KW_CHAR32_T, "KW_CHAR32_T"},
	{KW_CLASS, "KW_CLASS"},
	{KW_CONST, "KW_CONST"},
	{KW_CONSTEXPR, "KW_CONSTEXPR"},
	{KW_CONST_CAST, "KW_CONST_CAST"},
	{KW_CONTINUE, "KW_CONTINUE"},
	{KW_DECLTYPE, "KW_DECLTYPE"},
	{KW_DEFAULT, "KW_DEFAULT"},
	{KW_DELETE, "KW_DELETE"},
	{KW_DO, "KW_DO"},
	{KW_DOUBLE, "KW_DOUBLE"},
	{KW_DYNAMIC_CAST, "KW_DYNAMIC_CAST"},
	{KW_ELSE, "KW_ELSE"},
	{KW_ENUM, "KW_ENUM"},
	{KW_EXPLICIT, "KW_EXPLICIT"},
	{KW_EXPORT, "KW_EXPORT"},
	{KW_EXTERN, "KW_EXTERN"},
	{KW_FALSE, "KW_FALSE"},
	{KW_FLOAT, "KW_FLOAT"},
	{KW_FOR, "KW_FOR"},
	{KW_FRIEND, "KW_FRIEND"},
	{KW_GOTO, "KW_GOTO"},
	{KW_IF, "KW_IF"},
	{KW_INLINE, "KW_INLINE"},
	{KW_INT, "KW_INT"},
	{KW_LONG, "KW_LONG"},
	{KW_MUTABLE, "KW_MUTABLE"},
	{KW_NAMESPACE, "KW_NAMESPACE"},
	{KW_NEW, "KW_NEW"},
	{KW_NOEXCEPT, "KW_NOEXCEPT"},
	{KW_NULLPTR, "KW_NULLPTR"},
	{KW_OPERATOR, "KW_OPERATOR"},
	{KW_PRIVATE, "KW_PRIVATE"},
	{KW_PROTECTED, "KW_PROTECTED"},
	{KW_PUBLIC, "KW_PUBLIC"},
	{KW_REGISTER, "KW_REGISTER"},
	{KW_REINTERPET_CAST, "KW_REINTERPET_CAST"},
	{KW_RETURN, "KW_RETURN"},
	{KW_SHORT, "KW_SHORT"},
	{KW_SIGNED, "KW_SIGNED"},
	{KW_SIZEOF, "KW_SIZEOF"},
	{KW_STATIC, "KW_STATIC"},
	{KW_STATIC_ASSERT, "KW_STATIC_ASSERT"},
	{KW_STATIC_CAST, "KW_STATIC_CAST"},
	{KW_STRUCT, "KW_STRUCT"},
	{KW_SWITCH, "KW_SWITCH"},
	{KW_TEMPLATE, "KW_TEMPLATE"},
	{KW_THIS, "KW_THIS"},
	{KW_THREAD_LOCAL, "KW_THREAD_LOCAL"},
	{KW_THROW, "KW_THROW"},
	{KW_TRUE, "KW_TRUE"},
	{KW_TRY, "KW_TRY"},
	{KW_TYPEDEF, "KW_TYPEDEF"},
	{KW_TYPEID, "KW_TYPEID"},
	{KW_TYPENAME, "KW_TYPENAME"},
	{KW_UNION, "KW_UNION"},
	{KW_UNSIGNED, "KW_UNSIGNED"},
	{KW_USING, "KW_USING"},
	{KW_VIRTUAL, "KW_VIRTUAL"},
	{KW_VOID, "KW_VOID"},
	{KW_VOLATILE, "KW_VOLATILE"},
	{KW_WCHAR_T, "KW_WCHAR_T"},
	{KW_WHILE, "KW_WHILE"},
	{OP_LBRACE, "OP_LBRACE"},
	{OP_RBRACE, "OP_RBRACE"},
	{OP_LSQUARE, "OP_LSQUARE"},
	{OP_RSQUARE, "OP_RSQUARE"},
	{OP_LPAREN, "OP_LPAREN"},
	{OP_RPAREN, "OP_RPAREN"},
	{OP_BOR, "OP_BOR"},
	{OP_XOR, "OP_XOR"},
	{OP_COMPL, "OP_COMPL"},
	{OP_AMP, "OP_AMP"},
	{OP_LNOT, "OP_LNOT"},
	{OP_SEMICOLON, "OP_SEMICOLON"},
	{OP_COLON, "OP_COLON"},
	{OP_DOTS, "OP_DOTS"},
	{OP_QMARK, "OP_QMARK"},
	{OP_COLON2, "OP_COLON2"},
	{OP_DOT, "OP_DOT"},
	{OP_DOTSTAR, "OP_DOTSTAR"},
	{OP_PLUS, "OP_PLUS"},
	{OP_MINUS, "OP_MINUS"},
	{OP_STAR, "OP_STAR"},
	{OP_DIV, "OP_DIV"},
	{OP_MOD, "OP_MOD"},
	{OP_ASS, "OP_ASS"},
	{OP_LT, "OP_LT"},
	{OP_GT, "OP_GT"},
	{OP_PLUSASS, "OP_PLUSASS"},
	{OP_MINUSASS, "OP_MINUSASS"},
	{OP_STARASS, "OP_STARASS"},
	{OP_DIVASS, "OP_DIVASS"},
	{OP_MODASS, "OP_MODASS"},
	{OP_XORASS, "OP_XORASS"},
	{OP_BANDASS, "OP_BANDASS"},
	{OP_BORASS, "OP_BORASS"},
	{OP_LSHIFT, "OP_LSHIFT"},
	{OP_RSHIFT, "OP_RSHIFT"},
	{OP_RSHIFTASS, "OP_RSHIFTASS"},
	{OP_LSHIFTASS, "OP_LSHIFTASS"},
	{OP_EQ, "OP_EQ"},
	{OP_NE, "OP_NE"},
	{OP_LE, "OP_LE"},
	{OP_GE, "OP_GE"},
	{OP_LAND, "OP_LAND"},
	{OP_LOR, "OP_LOR"},
	{OP_INC, "OP_INC"},
	{OP_DEC, "OP_DEC"},
	{OP_COMMA, "OP_COMMA"},
	{OP_ARROWSTAR, "OP_ARROWSTAR"},
	{OP_ARROW, "OP_ARROW"}
};

// convert integer [0,15] to hexadecimal digit
char ValueToHexChar(int c)
{
	switch (c)
	{
	case 0: return '0';
	case 1: return '1';
	case 2: return '2';
	case 3: return '3';
	case 4: return '4';
	case 5: return '5';
	case 6: return '6';
	case 7: return '7';
	case 8: return '8';
	case 9: return '9';
	case 10: return 'A';
	case 11: return 'B';
	case 12: return 'C';
	case 13: return 'D';
	case 14: return 'E';
	case 15: return 'F';
	default: throw logic_error("ValueToHexChar of nonhex value");
	}
}

// hex dump memory range
string HexDump(const void* pdata, size_t nbytes)
{
	unsigned char* p = (unsigned char*) pdata;

	string s(nbytes*2, '?');

	for (size_t i = 0; i < nbytes; i++)
	{
		s[2*i+0] = ValueToHexChar((p[i] & 0xF0) >> 4);
		s[2*i+1] = ValueToHexChar((p[i] & 0x0F) >> 0);
	}

	return s;
}

// DebugPostTokenOutputStream: helper class to produce PA2 output format
struct DebugPostTokenOutputStream
{
	// output: invalid <source>
	void emit_invalid(const string& source)
	{
		cout << "invalid " << source << endl;
	}

	// output: simple <source> <token_type>
	void emit_simple(const string& source, ETokenType token_type)
	{
		cout << "simple " << source << " " << TokenTypeToStringMap.at(token_type) << endl;
	}

	// output: identifier <source>
	void emit_identifier(const string& source)
	{
		cout << "identifier " << source << endl;
	}

	// output: literal <source> <type> <hexdump(data,nbytes)>
	void emit_literal(const string& source, EFundamentalType type, const void* data, size_t nbytes)
	{
		cout << "literal " << source << " " << FundamentalTypeToStringMap.at(type) << " " << HexDump(data, nbytes) << endl;
	}

	// output: literal <source> array of <num_elements> <type> <hexdump(data,nbytes)>
	void emit_literal_array(const string& source, size_t num_elements, EFundamentalType type, const void* data, size_t nbytes)
	{
		cout << "literal " << source << " array of " << num_elements << " " << FundamentalTypeToStringMap.at(type) << " " << HexDump(data, nbytes) << endl;
	}

	// output: user-defined-literal <source> <ud_suffix> character <type> <hexdump(data,nbytes)>
	void emit_user_defined_literal_character(const string& source, const string& ud_suffix, EFundamentalType type, const void* data, size_t nbytes)
	{
		cout << "user-defined-literal " << source << " " << ud_suffix << " character " << FundamentalTypeToStringMap.at(type) << " " << HexDump(data, nbytes) << endl;
	}

	// output: user-defined-literal <source> <ud_suffix> string array of <num_elements> <type> <hexdump(data, nbytes)>
	void emit_user_defined_literal_string_array(const string& source, const string& ud_suffix, size_t num_elements, EFundamentalType type, const void* data, size_t nbytes)
	{
		cout << "user-defined-literal " << source << " " << ud_suffix << " string array of " << num_elements << " " << FundamentalTypeToStringMap.at(type) << " " << HexDump(data, nbytes) << endl;
	}

	// output: user-defined-literal <source> <ud_suffix> <prefix>
	void emit_user_defined_literal_integer(const string& source, const string& ud_suffix, const string& prefix)
	{
		cout << "user-defined-literal " << source << " " << ud_suffix << " integer " << prefix << endl;
	}

	// output: user-defined-literal <source> <ud_suffix> <prefix>
	void emit_user_defined_literal_floating(const string& source, const string& ud_suffix, const string& prefix)
	{
		cout << "user-defined-literal " << source << " " << ud_suffix << " floating " << prefix << endl;
	}

	// output : eof
	void emit_eof()
	{
		cout << "eof" << endl;
	}
};


// use these 3 functions to scan `floating-literals` (see PA2)
// for example PA2Decode_float("12.34") returns "12.34" as a `float` type
float PA2Decode_float(const string& s)
{
	istringstream iss(s);
	float x;
	iss >> x;
	return x;
}

double PA2Decode_double(const string& s)
{
	istringstream iss(s);
	double x;
	iss >> x;
	return x;
}

long double PA2Decode_long_double(const string& s)
{
	istringstream iss(s);
	long double x;
	iss >> x;
	return x;
}

// PA2's post-tokenizer is an event consumer: ordinary tokens are converted
// immediately, while only one maximal adjacent string run is retained.
namespace {

struct LiteralFailure {};

enum Encoding { ENC_ORDINARY, ENC_UTF8, ENC_UTF16, ENC_UTF32, ENC_WCHAR };

struct StringPiece
{
  string source;
  bool user;
  StringPiece(const string & s, bool u) : source(s), user(u) {}
};

struct DecodedValue
{
  uint32_t value;
  bool numeric_escape;
  DecodedValue(uint32_t v = 0, bool n = false) : value(v), numeric_escape(n) {}
};

bool ascii_digit(char c) { return c >= '0' && c <= '9'; }
bool ascii_hex(char c)
{
  return ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
int hex_digit(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool decode_utf8_at(const string & text, size_t at, uint32_t * cp, size_t * next)
{
  if (at >= text.size()) return false;
  const unsigned char first = static_cast<unsigned char>(text[at]);
  if (first < 0x80)
  {
    *cp = first;
    *next = at + 1;
    return true;
  }
  unsigned count;
  uint32_t value;
  if (first >= 0xc2 && first <= 0xdf) { count = 2; value = first & 0x1f; }
  else if (first >= 0xe0 && first <= 0xef) { count = 3; value = first & 0x0f; }
  else if (first >= 0xf0 && first <= 0xf4) { count = 4; value = first & 0x07; }
  else return false;
  if (at + count > text.size()) return false;
  for (unsigned i = 1; i < count; ++i)
  {
    const unsigned char c = static_cast<unsigned char>(text[at + i]);
    if ((c & 0xc0) != 0x80) return false;
    if (i == 1 && ((first == 0xe0 && c < 0xa0) ||
                   (first == 0xed && c >= 0xa0) ||
                   (first == 0xf0 && c < 0x90) ||
                   (first == 0xf4 && c >= 0x90))) return false;
    value = (value << 6) | (c & 0x3f);
  }
  if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
  *cp = value;
  *next = at + count;
  return true;
}

void append_utf8(uint32_t cp, string * out)
{
  if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) throw LiteralFailure();
  if (cp < 0x80) out->push_back(static_cast<char>(cp));
  else if (cp < 0x800)
  {
    out->push_back(static_cast<char>(0xc0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
  else if (cp < 0x10000)
  {
    out->push_back(static_cast<char>(0xe0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
  else
  {
    out->push_back(static_cast<char>(0xf0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
}

bool valid_ud_suffix(const string & suffix)
{
  if (suffix.size() < 2 || suffix[0] != '_') return false;
  size_t at = 1;
  while (at < suffix.size())
  {
    unsigned char c = static_cast<unsigned char>(suffix[at]);
    if (c < 0x80)
    {
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_')) return false;
      ++at;
    }
    else
    {
      uint32_t cp;
      size_t next;
      if (!decode_utf8_at(suffix, at, &cp, &next)) return false;
      at = next;
    }
  }
  return true;
}

bool is_keyword_or_operator(const string & source, ETokenType * type)
{
  unordered_map<string, ETokenType>::const_iterator it = StringToTokenTypeMap.find(source);
  if (it == StringToTokenTypeMap.end()) return false;
  *type = it->second;
  return true;
}

// The integer core is separated from suffix parsing so a numeric UDL cannot
// accidentally consume a standard integer suffix as part of its prefix.
bool parse_integer_core_syntax(const string & s, size_t * end, unsigned * base)
{
  if (s.empty()) return false;
  size_t i = 0;
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
  {
    *base = 16;
    i = 2;
    if (i == s.size() || !ascii_hex(s[i])) return false;
  }
  else if (s[0] == '0') *base = 8;
  else
  {
    if (s[0] < '1' || s[0] > '9') return false;
    *base = 10;
  }
  const size_t first = i;
  for (; i < s.size(); ++i)
  {
    int digit = -1;
    if (s[i] >= '0' && s[i] <= '9') digit = s[i] - '0';
    else if (*base == 16) digit = hex_digit(s[i]);
    if (digit < 0 || static_cast<unsigned>(digit) >= *base) break;
  }
  if (i == first) return false;
  *end = i;
  return true;
}

bool parse_integer_core(const string & s, size_t * end, unsigned * base,
                        uint64_t * value)
{
  if (s.empty()) return false;
  size_t i = 0;
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
  {
    *base = 16;
    i = 2;
    if (i == s.size() || !ascii_hex(s[i])) return false;
  }
  else if (s[0] == '0')
  {
    *base = 8;
    i = 0;
  }
  else
  {
    if (s[0] < '1' || s[0] > '9') return false;
    *base = 10;
  }
  const size_t first_digit = i;
  uint64_t v = 0;
  for (; i < s.size(); ++i)
  {
    int digit = -1;
    if (s[i] >= '0' && s[i] <= '9') digit = s[i] - '0';
    else if (*base == 16) digit = hex_digit(s[i]);
    if (digit < 0 || static_cast<unsigned>(digit) >= *base) break;
    if (v > (numeric_limits<uint64_t>::max() - static_cast<unsigned>(digit)) / *base)
      return false;
    v = v * *base + static_cast<unsigned>(digit);
  }
  if (i == first_digit) return false;
  *end = i;
  *value = v;
  return true;
}

bool parse_integer_suffix(const string & s, size_t at, string * suffix)
{
  string lowered;
  for (size_t i = at; i < s.size(); ++i)
  {
    if (i + 1 < s.size() && (s[i] == 'l' || s[i] == 'L') &&
        (s[i + 1] == 'l' || s[i + 1] == 'L') && s[i] != s[i + 1])
      return false;
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    lowered.push_back(c);
  }
  if (lowered.empty() || lowered == "u" || lowered == "l" || lowered == "ll" ||
      lowered == "ul" || lowered == "lu" || lowered == "ull" || lowered == "llu")
  {
    *suffix = lowered;
    return true;
  }
  return false;
}

bool parse_decimal_float_core(const string & s, size_t * end, bool * is_float)
{
  size_t i = 0;
  bool digits_before = false;
  while (i < s.size() && ascii_digit(s[i])) { digits_before = true; ++i; }
  bool dot = false;
  if (i < s.size() && s[i] == '.')
  {
    dot = true;
    ++i;
    const size_t after_dot = i;
    while (i < s.size() && ascii_digit(s[i])) ++i;
    if (!digits_before && i == after_dot) return false;
  }
  else if (!digits_before) return false;

  bool exponent = false;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
  {
    exponent = true;
    ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    const size_t exponent_start = i;
    while (i < s.size() && ascii_digit(s[i])) ++i;
    if (i == exponent_start) return false;
  }
  if (!dot && !exponent) return false;
  *end = i;
  *is_float = true;
  return true;
}

bool parse_floating_literal(const string & s, size_t * end, char * suffix)
{
  bool ignored;
  size_t core_end;
  if (!parse_decimal_float_core(s, &core_end, &ignored)) return false;
  size_t i = core_end;
  char suffix_char = 0;
  if (i < s.size() && (s[i] == 'f' || s[i] == 'F' || s[i] == 'l' || s[i] == 'L'))
    suffix_char = s[i++];
  *end = i;
  *suffix = suffix_char;
  return true;
}

bool integer_type_for(uint64_t value, unsigned base, const string & suffix,
                      EFundamentalType * type)
{
  vector<EFundamentalType> candidates;
  const bool uns = suffix.find('u') != string::npos;
  const bool is_ll = suffix.find("ll") != string::npos;
  const bool is_l = !is_ll && suffix.find('l') != string::npos;
  if (uns)
  {
    if (is_ll) candidates.push_back(FT_UNSIGNED_LONG_LONG_INT);
    else if (is_l)
    {
      candidates.push_back(FT_UNSIGNED_LONG_INT);
      candidates.push_back(FT_UNSIGNED_LONG_LONG_INT);
    }
    else
    {
      candidates.push_back(FT_UNSIGNED_INT);
      candidates.push_back(FT_UNSIGNED_LONG_INT);
      candidates.push_back(FT_UNSIGNED_LONG_LONG_INT);
    }
  }
  else if (is_ll)
  {
    candidates.push_back(FT_LONG_LONG_INT);
    if (base != 10) candidates.push_back(FT_UNSIGNED_LONG_LONG_INT);
  }
  else if (is_l)
  {
    candidates.push_back(FT_LONG_INT);
    if (base != 10) candidates.push_back(FT_UNSIGNED_LONG_INT);
    candidates.push_back(FT_LONG_LONG_INT);
    if (base != 10) candidates.push_back(FT_UNSIGNED_LONG_LONG_INT);
  }
  else
  {
    candidates.push_back(FT_INT);
    if (base != 10) candidates.push_back(FT_UNSIGNED_INT);
    candidates.push_back(FT_LONG_INT);
    if (base != 10) candidates.push_back(FT_UNSIGNED_LONG_INT);
    candidates.push_back(FT_LONG_LONG_INT);
    if (base != 10) candidates.push_back(FT_UNSIGNED_LONG_LONG_INT);
  }
  for (size_t i = 0; i < candidates.size(); ++i)
  {
    EFundamentalType candidate = candidates[i];
    const bool unsigned_type = candidate == FT_UNSIGNED_INT ||
      candidate == FT_UNSIGNED_LONG_INT || candidate == FT_UNSIGNED_LONG_LONG_INT;
    unsigned bits = candidate == FT_INT || candidate == FT_UNSIGNED_INT ? 32 : 64;
    uint64_t limit = unsigned_type ? numeric_limits<uint64_t>::max()
      : ((uint64_t(1) << (bits - 1)) - 1);
    if (bits == 32 && unsigned_type) limit = 0xffffffffULL;
    if (value <= limit) { *type = candidate; return true; }
  }
  return false;
}

void emit_integer_literal(DebugPostTokenOutputStream & output,
                          const string & source, EFundamentalType type,
                          uint64_t value)
{
  unsigned char bytes[8];
  size_t count;
  switch (type)
  {
    case FT_INT: case FT_UNSIGNED_INT: count = 4; break;
    case FT_LONG_INT: case FT_UNSIGNED_LONG_INT:
    case FT_LONG_LONG_INT: case FT_UNSIGNED_LONG_LONG_INT: count = 8; break;
    default: throw logic_error("unexpected integer literal type");
  }
  for (size_t i = 0; i < count; ++i) bytes[i] = static_cast<unsigned char>(value >> (8 * i));
  output.emit_literal(source, type, bytes, count);
}

bool try_ud_number(const string & s, DebugPostTokenOutputStream & output)
{
  for (size_t split = 1; split < s.size(); ++split)
  {
    if (s[split] != '_') continue;
    const string suffix = s.substr(split);
    if (!valid_ud_suffix(suffix)) continue;
    const string prefix = s.substr(0, split);
    size_t end = 0;
    unsigned base = 10;
    if (parse_integer_core_syntax(prefix, &end, &base) && end == prefix.size())
    {
      output.emit_user_defined_literal_integer(s, suffix, prefix);
      return true;
    }
    bool is_float = false;
    if (parse_decimal_float_core(prefix, &end, &is_float) && end == prefix.size())
    {
      output.emit_user_defined_literal_floating(s, suffix, prefix);
      return true;
    }
  }
  return false;
}

void posttoken_number(const string & s, DebugPostTokenOutputStream & output)
{
  if (try_ud_number(s, output)) return;
  size_t end = 0;
  char float_suffix = 0;
  if (parse_floating_literal(s, &end, &float_suffix) && end == s.size())
  {
    const string core = float_suffix ? s.substr(0, s.size() - 1) : s;
    if (float_suffix == 'f' || float_suffix == 'F')
    {
      float value = PA2Decode_float(core);
      output.emit_literal(s, FT_FLOAT, &value, sizeof(value));
    }
    else if (float_suffix == 'l' || float_suffix == 'L')
    {
      long double value;
      memset(&value, 0, sizeof(value));
      value = PA2Decode_long_double(core);
      output.emit_literal(s, FT_LONG_DOUBLE, &value, sizeof(value));
    }
    else
    {
      double value = PA2Decode_double(core);
      output.emit_literal(s, FT_DOUBLE, &value, sizeof(value));
    }
    return;
  }
  string integer_suffix;
  unsigned base = 10;
  uint64_t value = 0;
  if (!parse_integer_core(s, &end, &base, &value) ||
      !parse_integer_suffix(s, end, &integer_suffix))
  {
    output.emit_invalid(s);
    return;
  }
  EFundamentalType type;
  if (!integer_type_for(value, base, integer_suffix, &type))
  {
    output.emit_invalid(s);
    return;
  }
  emit_integer_literal(output, s, type, value);
}

// Literal body parsing preserves the distinction between a Unicode scalar
// (which is encoded) and a numeric escape (which contributes one code unit).
void decode_literal_body(const string & body, bool raw,
                         vector<DecodedValue> * values)
{
  size_t i = 0;
  while (i < body.size())
  {
    if (raw || body[i] != '\\')
    {
      uint32_t cp;
      size_t next;
      if (!decode_utf8_at(body, i, &cp, &next)) throw LiteralFailure();
      values->push_back(DecodedValue(cp, false));
      i = next;
      continue;
    }
    ++i;
    if (i == body.size()) throw LiteralFailure();
    const char c = body[i++];
    if (c == 'x')
    {
      if (i == body.size() || !ascii_hex(body[i])) throw LiteralFailure();
      uint64_t value = 0;
      while (i < body.size() && ascii_hex(body[i]))
      {
        int digit = hex_digit(body[i++]);
        if (value > (numeric_limits<uint32_t>::max() - static_cast<unsigned>(digit)) / 16)
          throw LiteralFailure();
        value = value * 16 + static_cast<unsigned>(digit);
      }
      values->push_back(DecodedValue(static_cast<uint32_t>(value), true));
      continue;
    }
    if (c >= '0' && c <= '7')
    {
      unsigned value = static_cast<unsigned>(c - '0');
      unsigned count = 1;
      while (count < 3 && i < body.size() && body[i] >= '0' && body[i] <= '7')
      {
        value = value * 8 + static_cast<unsigned>(body[i++] - '0');
        ++count;
      }
      values->push_back(DecodedValue(value, true));
      continue;
    }
    uint32_t value;
    switch (c)
    {
      case '\'': value = '\''; break;
      case '"': value = '"'; break;
      case '?': value = '?'; break;
      case '\\': value = '\\'; break;
      case 'a': value = 7; break;
      case 'b': value = 8; break;
      case 'f': value = 12; break;
      case 'n': value = 10; break;
      case 'r': value = 13; break;
      case 't': value = 9; break;
      case 'v': value = 11; break;
      default: throw LiteralFailure();
    }
    values->push_back(DecodedValue(value, false));
  }
}

void append_code_unit(vector<unsigned char> * bytes, uint32_t value, unsigned width)
{
  for (unsigned i = 0; i < width; ++i)
    bytes->push_back(static_cast<unsigned char>(value >> (8 * i)));
}

void encode_value(const DecodedValue & item, Encoding encoding,
                  vector<unsigned char> * bytes)
{
  const unsigned width = encoding == ENC_UTF16 ? 2 :
    (encoding == ENC_UTF32 || encoding == ENC_WCHAR ? 4 : 1);
  const uint64_t max_value = width == 1 ? 0xffULL :
    (width == 2 ? 0xffffULL : 0xffffffffULL);
  if (item.numeric_escape)
  {
    if (item.value > max_value) throw LiteralFailure();
    append_code_unit(bytes, item.value, width);
    return;
  }
  const uint32_t cp = item.value;
  if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) throw LiteralFailure();
  if (encoding == ENC_UTF16 && cp > 0xffff)
  {
    const uint32_t adjusted = cp - 0x10000;
    append_code_unit(bytes, 0xd800 + (adjusted >> 10), 2);
    append_code_unit(bytes, 0xdc00 + (adjusted & 0x3ff), 2);
  }
  else if (encoding == ENC_UTF32 || encoding == ENC_WCHAR || encoding == ENC_UTF16)
    append_code_unit(bytes, cp, width);
  else
  {
    string encoded;
    append_utf8(cp, &encoded);
    for (size_t i = 0; i < encoded.size(); ++i)
      bytes->push_back(static_cast<unsigned char>(encoded[i]));
  }
}

struct StringDescriptor
{
  Encoding encoding;
  bool explicit_encoding;
  bool raw;
  string body;
  string suffix;
};

bool describe_string(const StringPiece & token, StringDescriptor * out)
{
  const string & source = token.source;
  size_t quote = string::npos;
  string prefix;
  const char * prefixes[] = {"u8R\"", "uR\"", "UR\"", "LR\"", "R\"",
                             "u8\"", "u\"", "U\"", "L\"", "\""};
  for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); ++p)
  {
    string candidate(prefixes[p]);
    if (source.compare(0, candidate.size(), candidate) == 0)
    {
      prefix = candidate;
      quote = candidate.find('"');
      break;
    }
  }
  if (quote == string::npos) return false;
  const bool raw = prefix.find('R') != string::npos;
  out->raw = raw;
  out->explicit_encoding = prefix[0] != 'R' && prefix[0] != '"';
  if (prefix.compare(0, 2, "u8") == 0) out->encoding = ENC_UTF8;
  else if (prefix[0] == 'u') out->encoding = ENC_UTF16;
  else if (prefix[0] == 'U') out->encoding = ENC_UTF32;
  else if (prefix[0] == 'L') out->encoding = ENC_WCHAR;
  else out->encoding = ENC_ORDINARY;

  size_t close_end = string::npos;
  if (raw)
  {
    const size_t open = source.find('(', quote + 1);
    if (open == string::npos) return false;
    const string delimiter = source.substr(quote + 1, open - quote - 1);
    const string terminator = ")" + delimiter + "\"";
    const size_t close = source.find(terminator, open + 1);
    if (close == string::npos) return false;
    out->body = source.substr(open + 1, close - open - 1);
    close_end = close + terminator.size();
  }
  else
  {
    size_t i = quote + 1;
    while (i < source.size())
    {
      if (source[i] == '\\')
      {
        ++i;
        if (i == source.size()) return false;
        uint32_t cp;
        size_t next;
        if (decode_utf8_at(source, i, &cp, &next)) i = next;
        else ++i;
        continue;
      }
      if (source[i] == '"')
      {
        out->body = source.substr(quote + 1, i - quote - 1);
        close_end = i + 1;
        break;
      }
      uint32_t cp;
      size_t next;
      if (!decode_utf8_at(source, i, &cp, &next)) return false;
      i = next;
    }
    if (close_end == string::npos) return false;
  }
  out->suffix = source.substr(close_end);
  if ((!token.user && !out->suffix.empty()) ||
      (token.user && (!valid_ud_suffix(out->suffix) || out->suffix.empty()))) return false;
  return true;
}

void posttoken_string_run(const vector<StringPiece> & run,
                          DebugPostTokenOutputStream & output)
{
  string source;
  for (size_t i = 0; i < run.size(); ++i)
  {
    if (i) source.push_back(' ');
    source += run[i].source;
  }
  try
  {
    vector<StringDescriptor> descriptions(run.size());
    bool selected = false;
    Encoding encoding = ENC_ORDINARY;
    bool any_ud = false;
    string ud_suffix;
    for (size_t i = 0; i < run.size(); ++i)
    {
      if (!describe_string(run[i], &descriptions[i])) throw LiteralFailure();
      StringDescriptor & d = descriptions[i];
      if (d.explicit_encoding)
      {
        if (!selected) { encoding = d.encoding; selected = true; }
        else if (encoding != d.encoding) throw LiteralFailure();
      }
      if (!d.suffix.empty())
      {
        if (!any_ud) { ud_suffix = d.suffix; any_ud = true; }
        else if (ud_suffix != d.suffix) throw LiteralFailure();
      }
    }
    if (!selected) encoding = ENC_ORDINARY;
    vector<unsigned char> bytes;
    const unsigned width = encoding == ENC_UTF16 ? 2 :
      (encoding == ENC_UTF32 || encoding == ENC_WCHAR ? 4 : 1);
    for (size_t i = 0; i < descriptions.size(); ++i)
    {
      vector<DecodedValue> values;
      decode_literal_body(descriptions[i].body, descriptions[i].raw, &values);
      for (size_t j = 0; j < values.size(); ++j)
        encode_value(values[j], encoding, &bytes);
    }
    append_code_unit(&bytes, 0, width);
    const size_t elements = bytes.size() / width;
    EFundamentalType type = encoding == ENC_UTF16 ? FT_CHAR16_T :
      (encoding == ENC_UTF32 ? FT_CHAR32_T :
       (encoding == ENC_WCHAR ? FT_WCHAR_T : FT_CHAR));
    if (any_ud)
      output.emit_user_defined_literal_string_array(source, ud_suffix, elements,
                                                     type, bytes.data(), bytes.size());
    else
      output.emit_literal_array(source, elements, type, bytes.data(), bytes.size());
  }
  catch (const LiteralFailure &)
  {
    output.emit_invalid(source);
  }
}

bool describe_character(const string & source, bool user, string * suffix,
                        Encoding * encoding, string * body)
{
  size_t quote = string::npos;
  if (!source.empty() && source[0] == '\'') { quote = 0; *encoding = ENC_ORDINARY; }
  else if (source.size() >= 2 && (source[0] == 'u' || source[0] == 'U' || source[0] == 'L') && source[1] == '\'')
  {
    quote = 1;
    *encoding = source[0] == 'u' ? ENC_UTF16 :
      (source[0] == 'U' ? ENC_UTF32 : ENC_WCHAR);
  }
  if (quote == string::npos) return false;
  size_t i = quote + 1;
  size_t closing = string::npos;
  while (i < source.size())
  {
    if (source[i] == '\\')
    {
      ++i;
      if (i == source.size()) return false;
      uint32_t cp;
      size_t next;
      if (decode_utf8_at(source, i, &cp, &next)) i = next;
      else ++i;
    }
    else if (source[i] == '\'') { closing = i; break; }
    else
    {
      uint32_t cp;
      size_t next;
      if (!decode_utf8_at(source, i, &cp, &next)) return false;
      i = next;
    }
  }
  if (closing == string::npos) return false;
  *body = source.substr(quote + 1, closing - quote - 1);
  *suffix = source.substr(closing + 1);
  if (user) return valid_ud_suffix(*suffix);
  return suffix->empty();
}

void posttoken_character(const string & source, bool user,
                         DebugPostTokenOutputStream & output)
{
  try
  {
    string suffix, body;
    Encoding encoding;
    if (!describe_character(source, user, &suffix, &encoding, &body)) throw LiteralFailure();
    vector<DecodedValue> values;
    decode_literal_body(body, false, &values);
    if (values.size() != 1) throw LiteralFailure();
    const uint32_t cp = values[0].value;
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) throw LiteralFailure();
    EFundamentalType type;
    uint64_t value = cp;
    if (encoding == ENC_ORDINARY)
      type = cp <= 127 ? FT_CHAR : FT_INT;
    else if (encoding == ENC_UTF16)
    {
      if (cp > 0xffff) throw LiteralFailure();
      type = FT_CHAR16_T;
    }
    else if (encoding == ENC_UTF32) type = FT_CHAR32_T;
    else type = FT_WCHAR_T;
    if (values[0].numeric_escape && cp > 0xffffffffU) throw LiteralFailure();
    if (user)
    {
      if (type == FT_CHAR16_T)
      {
        uint16_t data = static_cast<uint16_t>(value);
        output.emit_user_defined_literal_character(source, suffix, type, &data, sizeof(data));
      }
      else if (type == FT_CHAR32_T || type == FT_WCHAR_T)
      {
        uint32_t data = static_cast<uint32_t>(value);
        output.emit_user_defined_literal_character(source, suffix, type, &data, sizeof(data));
      }
      else if (type == FT_INT)
      {
        uint32_t data = static_cast<uint32_t>(value);
        output.emit_user_defined_literal_character(source, suffix, type, &data, sizeof(data));
      }
      else
      {
        uint8_t data = static_cast<uint8_t>(value);
        output.emit_user_defined_literal_character(source, suffix, type, &data, sizeof(data));
      }
    }
    else
    {
      if (type == FT_CHAR16_T)
      {
        uint16_t data = static_cast<uint16_t>(value);
        output.emit_literal(source, type, &data, sizeof(data));
      }
      else if (type == FT_CHAR32_T || type == FT_WCHAR_T)
      {
        uint32_t data = static_cast<uint32_t>(value);
        output.emit_literal(source, type, &data, sizeof(data));
      }
      else if (type == FT_INT)
      {
        uint32_t data = static_cast<uint32_t>(value);
        output.emit_literal(source, type, &data, sizeof(data));
      }
      else
      {
        uint8_t data = static_cast<uint8_t>(value);
        output.emit_literal(source, type, &data, sizeof(data));
      }
    }
  }
  catch (const LiteralFailure &)
  {
    output.emit_invalid(source);
  }
}

class PostTokenStream : public IPPTokenStream
{
public:
  explicit PostTokenStream(DebugPostTokenOutputStream & output)
    : output_(output), after_operator_keyword_(false) {}

  void emit_whitespace_sequence() {}
  void emit_new_line() {}
  void emit_header_name(const string & data)
    { after_operator_keyword_ = false; flush_strings(); output_.emit_invalid(data); }
  void emit_identifier(const string & data)
  {
    flush_strings();
    ETokenType type;
    if (is_keyword_or_operator(data, &type)) output_.emit_simple(data, type);
    else output_.emit_identifier(data);
    after_operator_keyword_ = data == "operator";
  }
  void emit_pp_number(const string & data)
    { after_operator_keyword_ = false; flush_strings(); posttoken_number(data, output_); }
  void emit_character_literal(const string & data)
    { after_operator_keyword_ = false; flush_strings(); posttoken_character(data, false, output_); }
  void emit_user_defined_character_literal(const string & data)
    { after_operator_keyword_ = false; flush_strings(); posttoken_character(data, true, output_); }
  void emit_string_literal(const string & data)
    { after_operator_keyword_ = false; strings_.push_back(StringPiece(data, false)); }
  void emit_user_defined_string_literal(const string & data)
  {
    if (after_operator_keyword_ && data.size() > 2 && data.compare(0, 2, "\"\"") == 0)
    {
      const string suffix = data.substr(2);
      after_operator_keyword_ = false;
      strings_.push_back(StringPiece("\"\"", false));
      flush_strings();
      ETokenType type;
      if (is_keyword_or_operator(suffix, &type)) output_.emit_simple(suffix, type);
      else output_.emit_identifier(suffix);
      return;
    }
    after_operator_keyword_ = false;
    strings_.push_back(StringPiece(data, true));
  }
  void emit_preprocessing_op_or_punc(const string & data)
  {
    after_operator_keyword_ = false;
    flush_strings();
    if (data == "#" || data == "##" || data == "%:" || data == "%:%:")
      output_.emit_invalid(data);
    else
    {
      ETokenType type;
      if (is_keyword_or_operator(data, &type)) output_.emit_simple(data, type);
      else output_.emit_invalid(data);
    }
  }
  void emit_non_whitespace_char(const string & data)
    { after_operator_keyword_ = false; flush_strings(); output_.emit_invalid(data); }
  void emit_eof() { after_operator_keyword_ = false; flush_strings(); output_.emit_eof(); }

private:
  DebugPostTokenOutputStream & output_;
  vector<StringPiece> strings_;
  bool after_operator_keyword_;
  void flush_strings()
  {
    if (strings_.empty()) return;
    posttoken_string_run(strings_, output_);
    strings_.clear();
  }
};

} // anonymous namespace


int main(int argc, char** argv)
{
  (void)argc;
  (void)argv;
  std::ios_base::sync_with_stdio(false);
  std::cin.tie(NULL);
  try
  {
    const string source((istreambuf_iterator<char>(cin)), istreambuf_iterator<char>());
    DebugPostTokenOutputStream output;
    PostTokenStream posttoken(output);
    PPTokenizer tokenizer(source, posttoken);
    tokenizer.tokenize();
    return EXIT_SUCCESS;
  }
  catch (const exception & e)
  {
    cerr << "ERROR: " << e.what() << endl;
    return EXIT_FAILURE;
  }
}
