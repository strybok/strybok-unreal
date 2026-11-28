// Standard library only: see the notes at the top of StrybokCore.h.

#include "StrybokCore.h"

#include <limits>
#include <map>
#include <random>

namespace strybok
{

const char* const kSdkVersion = "0.4.0";

IHttpTransport::~IHttpTransport() = default;
IScheduler::~IScheduler() = default;
IClock::~IClock() = default;
IRandom::~IRandom() = default;

namespace
{

const std::size_t kMaxSlugLength = 64;
const std::size_t kMaxSdkVersionLength = 32;
const int kMaxJsonDepth = 64;
const int kMaxRetriesLimit = 10;
const std::size_t kMaxRoutePathLength = 512;
const std::int64_t kMaxRetryAfterSeconds = 2147483647;
const int kMaxDeferredAttempts = 3;
const std::int64_t kSourceTimeoutMs = 10000;

bool IsAsciiSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

char AsciiLower(char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IsAsciiDigit(char c)
{
	return c >= '0' && c <= '9';
}

bool IsAsciiAlnum(char c)
{
	return IsAsciiDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

std::string ToLowerAscii(const std::string& value)
{
	std::string result = value;
	for (char& c : result)
	{
		c = AsciiLower(c);
	}
	return result;
}

bool StartsWithIgnoreCase(const std::string& value, const char* prefix)
{
	std::size_t index = 0;
	for (; prefix[index] != '\0'; ++index)
	{
		if (index >= value.size() || AsciiLower(value[index]) != prefix[index])
		{
			return false;
		}
	}
	return true;
}

bool Contains(const std::vector<std::string>& values, const std::string& value)
{
	for (const std::string& candidate : values)
	{
		if (candidate == value)
		{
			return true;
		}
	}
	return false;
}

bool IsValidHostName(const std::string& host)
{
	if (host.empty() || host.size() > 253)
	{
		return false;
	}
	for (char c : host)
	{
		if (!(IsAsciiDigit(c) || (c >= 'a' && c <= 'z') || c == '.' || c == '-'))
		{
			return false;
		}
	}
	return true;
}

/** `^/[A-Za-z0-9_-]{1,64}$` */
bool IsSlugPath(const std::string& path)
{
	if (path.size() < 2 || path.size() > kMaxSlugLength + 1 || path[0] != '/')
	{
		return false;
	}
	for (std::size_t index = 1; index < path.size(); ++index)
	{
		const char c = path[index];
		if (!(IsAsciiAlnum(c) || c == '_' || c == '-'))
		{
			return false;
		}
	}
	return true;
}

struct OurUrl
{
	std::string host;
	std::string path;
	/** Between `?` and `#` (or the end), without the `?`; empty when there is no query. */
	std::string rawQuery;
};

int HexValue(char c)
{
	if (c >= '0' && c <= '9')
	{
		return c - '0';
	}
	if (c >= 'a' && c <= 'f')
	{
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F')
	{
		return c - 'A' + 10;
	}
	return -1;
}

bool IsUnreserved(char c)
{
	return IsAsciiAlnum(c) || c == '.' || c == '_' || c == '~' || c == '-';
}

/**
 * Percent-decodes `text[begin, end)` (`+` is a space) into `out` and returns true only when the
 * result consists of `A-Z a-z 0-9 . _ ~ -`.
 *
 * Campaign keys and valid values are made of these characters only, so anything else - a space,
 * a malformed escape, any byte of a multi-byte UTF-8 sequence (valid or not), a NUL - can only
 * belong to a key that is not ours or to a value that is dropped. No UTF-8 decoding is needed
 * for that, and no byte of the input is ever trusted.
 */
bool DecodeUnreserved(const std::string& text, std::size_t begin, std::size_t end, std::string& out)
{
	out.clear();
	std::size_t i = begin;
	while (i < end)
	{
		char c = text[i];
		if (c == '%')
		{
			if (end - i < 3)
			{
				return false;
			}
			const int high = HexValue(text[i + 1]);
			const int low = HexValue(text[i + 2]);
			if (high < 0 || low < 0)
			{
				return false;
			}
			c = static_cast<char>(static_cast<unsigned char>(high * 16 + low));
			i += 3;
		}
		else
		{
			i += 1;
		}
		if (!IsUnreserved(c))
		{
			return false;
		}
		out.push_back(c);
	}
	return true;
}

const char* const kCampaignQueryKeys[] = {"utm_source", "utm_medium", "utm_campaign", "utm_term", "utm_content"};
const std::size_t kCampaignKeyCount = sizeof(kCampaignQueryKeys) / sizeof(kCampaignQueryKeys[0]);
const std::size_t kMaxCampaignValueLength = 64;

/**
 * The single place that checks the scheme, host, user info and port of a URL. Without `anyPath`
 * the path must be exactly one slug segment; with it (the `parameterLinks` option) any path is
 * returned as received, possibly empty, for the rules of `BuildParameterLinkLookup`.
 */
bool ParseOurUrl(const std::string& url, const std::vector<std::string>& normalizedHosts, bool anyPath, OurUrl& outParts)
{
	// Scheme: https only (case-insensitive, like every URL parser).
	if (!StartsWithIgnoreCase(url, "https://"))
	{
		return false;
	}
	const std::size_t authorityBegin = 8;
	std::size_t authorityEnd = url.find_first_of("/?#", authorityBegin);
	if (authorityEnd == std::string::npos)
	{
		authorityEnd = url.size();
	}
	const std::string authority = url.substr(authorityBegin, authorityEnd - authorityBegin);

	// No user info: "https://user:pw@host/" is never one of our links.
	if (authority.empty() || authority.find('@') != std::string::npos)
	{
		return false;
	}

	std::string host = authority;
	const std::size_t colon = authority.find(':');
	if (colon != std::string::npos)
	{
		// No port, or port 443.
		const std::string port = authority.substr(colon + 1);
		host = authority.substr(0, colon);
		if (port.empty() || port.size() > 5)
		{
			return false;
		}
		int portNumber = 0;
		for (char c : port)
		{
			if (!IsAsciiDigit(c))
			{
				return false;
			}
			portNumber = portNumber * 10 + (c - '0');
		}
		if (portNumber != 443)
		{
			return false;
		}
	}

	host = ToLowerAscii(host);
	if (!IsValidHostName(host) || !Contains(normalizedHosts, host))
	{
		return false;
	}

	// Exactly one slug segment; query and fragment do not decide anything. (The path starts right
	// after the authority, so with `anyPath` it is empty or starts with "/".)
	std::size_t pathEnd = url.find_first_of("?#", authorityEnd);
	if (pathEnd == std::string::npos)
	{
		pathEnd = url.size();
	}
	const std::string path = url.substr(authorityEnd, pathEnd - authorityEnd);
	if (!anyPath && !IsSlugPath(path))
	{
		return false;
	}

	outParts.host = host;
	outParts.path = path;
	outParts.rawQuery.clear();
	if (pathEnd < url.size() && url[pathEnd] == '?')
	{
		std::size_t queryEnd = url.find('#', pathEnd);
		if (queryEnd == std::string::npos)
		{
			queryEnd = url.size();
		}
		outParts.rawQuery = url.substr(pathEnd + 1, queryEnd - pathEnd - 1);
	}
	return true;
}

std::string TrimTrailingSlashes(const std::string& value)
{
	std::size_t end = value.size();
	while (end > 0 && value[end - 1] == '/')
	{
		--end;
	}
	return value.substr(0, end);
}

std::string IntToString(int value)
{
	if (value == 0)
	{
		return "0";
	}
	const bool negative = value < 0;
	// Work in unsigned space so the most negative value is handled too.
	unsigned int magnitude = negative ? (0u - static_cast<unsigned int>(value)) : static_cast<unsigned int>(value);
	std::string digits;
	while (magnitude > 0)
	{
		digits.insert(digits.begin(), static_cast<char>('0' + static_cast<int>(magnitude % 10u)));
		magnitude /= 10u;
	}
	return negative ? "-" + digits : digits;
}

LinkError MakeError(ErrorKind kind, const std::string& message, const std::string& url, int statusCode = 0)
{
	LinkError error;
	error.kind = kind;
	error.message = message;
	error.url = url;
	error.statusCode = statusCode;
	return error;
}

// --- JSON ------------------------------------------------------------------------------------

void AppendUtf8(std::string& out, std::uint32_t codePoint)
{
	if (codePoint < 0x80u)
	{
		out.push_back(static_cast<char>(codePoint));
	}
	else if (codePoint < 0x800u)
	{
		out.push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
		out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
	}
	else if (codePoint < 0x10000u)
	{
		out.push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
		out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
		out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
	}
	else
	{
		out.push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
		out.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
		out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
		out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
	}
}

class JsonParser
{
public:
	explicit JsonParser(const std::string& text) : text_(text) {}

	bool ParseDocument(json::Value& outValue)
	{
		SkipWhitespace();
		if (!ParseValue(outValue, 0))
		{
			return false;
		}
		SkipWhitespace();
		return position_ == text_.size();
	}

private:
	bool AtEnd() const { return position_ >= text_.size(); }
	char Peek() const { return text_[position_]; }

	void SkipWhitespace()
	{
		while (!AtEnd())
		{
			const char c = Peek();
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
			{
				break;
			}
			++position_;
		}
	}

	bool ConsumeLiteral(const char* literal)
	{
		std::size_t length = 0;
		while (literal[length] != '\0')
		{
			++length;
		}
		if (text_.compare(position_, length, literal) != 0)
		{
			return false;
		}
		position_ += length;
		return true;
	}

	bool ParseValue(json::Value& outValue, int depth)
	{
		if (AtEnd() || depth > kMaxJsonDepth)
		{
			return false;
		}
		outValue = json::Value();
		outValue.begin = position_;
		bool ok = false;
		switch (Peek())
		{
		case '{':
			ok = ParseObject(outValue, depth);
			break;
		case '[':
			ok = ParseArray(outValue, depth);
			break;
		case '"':
			outValue.type = json::Type::String;
			ok = ParseString(outValue.text);
			break;
		case 't':
			outValue.type = json::Type::Bool;
			outValue.boolean = true;
			ok = ConsumeLiteral("true");
			break;
		case 'f':
			outValue.type = json::Type::Bool;
			ok = ConsumeLiteral("false");
			break;
		case 'n':
			outValue.type = json::Type::Null;
			ok = ConsumeLiteral("null");
			break;
		default:
			outValue.type = json::Type::Number;
			ok = ParseNumber(outValue.text);
			break;
		}
		outValue.end = position_;
		return ok;
	}

	bool ParseObject(json::Value& outValue, int depth)
	{
		outValue.type = json::Type::Object;
		++position_; // {
		SkipWhitespace();
		if (!AtEnd() && Peek() == '}')
		{
			++position_;
			return true;
		}
		for (;;)
		{
			SkipWhitespace();
			if (AtEnd() || Peek() != '"')
			{
				return false;
			}
			std::string key;
			if (!ParseString(key))
			{
				return false;
			}
			SkipWhitespace();
			if (AtEnd() || Peek() != ':')
			{
				return false;
			}
			++position_;
			SkipWhitespace();
			json::Value member;
			if (!ParseValue(member, depth + 1))
			{
				return false;
			}
			outValue.members.emplace_back(std::move(key), std::move(member));
			SkipWhitespace();
			if (AtEnd())
			{
				return false;
			}
			if (Peek() == ',')
			{
				++position_;
				continue;
			}
			if (Peek() == '}')
			{
				++position_;
				return true;
			}
			return false;
		}
	}

	bool ParseArray(json::Value& outValue, int depth)
	{
		outValue.type = json::Type::Array;
		++position_; // [
		SkipWhitespace();
		if (!AtEnd() && Peek() == ']')
		{
			++position_;
			return true;
		}
		for (;;)
		{
			SkipWhitespace();
			json::Value item;
			if (!ParseValue(item, depth + 1))
			{
				return false;
			}
			outValue.items.push_back(std::move(item));
			SkipWhitespace();
			if (AtEnd())
			{
				return false;
			}
			if (Peek() == ',')
			{
				++position_;
				continue;
			}
			if (Peek() == ']')
			{
				++position_;
				return true;
			}
			return false;
		}
	}

	bool ParseHex4(std::uint32_t& outCodeUnit)
	{
		if (text_.size() - position_ < 4)
		{
			return false;
		}
		std::uint32_t result = 0;
		for (int index = 0; index < 4; ++index)
		{
			const char c = text_[position_++];
			std::uint32_t digit = 0;
			if (c >= '0' && c <= '9')
			{
				digit = static_cast<std::uint32_t>(c - '0');
			}
			else if (c >= 'a' && c <= 'f')
			{
				digit = static_cast<std::uint32_t>(c - 'a' + 10);
			}
			else if (c >= 'A' && c <= 'F')
			{
				digit = static_cast<std::uint32_t>(c - 'A' + 10);
			}
			else
			{
				return false;
			}
			result = (result << 4) | digit;
		}
		outCodeUnit = result;
		return true;
	}

	bool ParseString(std::string& outText)
	{
		++position_; // opening quote
		outText.clear();
		for (;;)
		{
			if (AtEnd())
			{
				return false;
			}
			const char c = text_[position_++];
			if (c == '"')
			{
				return true;
			}
			if (static_cast<unsigned char>(c) < 0x20)
			{
				return false; // raw control characters are not allowed inside strings
			}
			if (c != '\\')
			{
				outText.push_back(c);
				continue;
			}
			if (AtEnd())
			{
				return false;
			}
			const char escape = text_[position_++];
			switch (escape)
			{
			case '"':
				outText.push_back('"');
				break;
			case '\\':
				outText.push_back('\\');
				break;
			case '/':
				outText.push_back('/');
				break;
			case 'b':
				outText.push_back('\b');
				break;
			case 'f':
				outText.push_back('\f');
				break;
			case 'n':
				outText.push_back('\n');
				break;
			case 'r':
				outText.push_back('\r');
				break;
			case 't':
				outText.push_back('\t');
				break;
			case 'u':
			{
				std::uint32_t unit = 0;
				if (!ParseHex4(unit))
				{
					return false;
				}
				std::uint32_t codePoint = unit;
				if (unit >= 0xD800u && unit <= 0xDBFFu)
				{
					// High surrogate: combine with a following low surrogate when there is one.
					codePoint = 0xFFFDu;
					if (text_.size() - position_ >= 6 && text_[position_] == '\\' && text_[position_ + 1] == 'u')
					{
						const std::size_t rewind = position_;
						position_ += 2;
						std::uint32_t low = 0;
						if (!ParseHex4(low))
						{
							return false;
						}
						if (low >= 0xDC00u && low <= 0xDFFFu)
						{
							codePoint = 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u);
						}
						else
						{
							position_ = rewind; // not a pair: decode the next escape on its own
						}
					}
				}
				else if (unit >= 0xDC00u && unit <= 0xDFFFu)
				{
					codePoint = 0xFFFDu; // lone low surrogate
				}
				AppendUtf8(outText, codePoint);
				break;
			}
			default:
				return false;
			}
		}
	}

	bool ParseNumber(std::string& outText)
	{
		const std::size_t start = position_;
		if (!AtEnd() && Peek() == '-')
		{
			++position_;
		}
		if (AtEnd() || !IsAsciiDigit(Peek()))
		{
			return false;
		}
		if (Peek() == '0')
		{
			++position_;
		}
		else
		{
			while (!AtEnd() && IsAsciiDigit(Peek()))
			{
				++position_;
			}
		}
		if (!AtEnd() && Peek() == '.')
		{
			++position_;
			if (AtEnd() || !IsAsciiDigit(Peek()))
			{
				return false;
			}
			while (!AtEnd() && IsAsciiDigit(Peek()))
			{
				++position_;
			}
		}
		if (!AtEnd() && (Peek() == 'e' || Peek() == 'E'))
		{
			++position_;
			if (!AtEnd() && (Peek() == '+' || Peek() == '-'))
			{
				++position_;
			}
			if (AtEnd() || !IsAsciiDigit(Peek()))
			{
				return false;
			}
			while (!AtEnd() && IsAsciiDigit(Peek()))
			{
				++position_;
			}
		}
		outText = text_.substr(start, position_ - start);
		return true;
	}

	const std::string& text_;
	std::size_t position_ = 0;
};

class StdRandom final : public IRandom
{
public:
	void Fill(std::uint8_t* bytes, std::size_t count) override
	{
		std::size_t index = 0;
		while (index < count)
		{
			unsigned int word = device_();
			for (int part = 0; part < 4 && index < count; ++part)
			{
				bytes[index++] = static_cast<std::uint8_t>(word & 0xFFu);
				word >>= 8;
			}
		}
	}

private:
	std::random_device device_;
};

} // namespace

// ---------------------------------------------------------------------------------------------
// Pure rules
// ---------------------------------------------------------------------------------------------

std::vector<std::string> NormalizeHosts(const std::vector<std::string>& hosts)
{
	std::vector<std::string> result;
	for (const std::string& raw : hosts)
	{
		std::size_t begin = 0;
		std::size_t end = raw.size();
		while (begin < end && IsAsciiSpace(raw[begin]))
		{
			++begin;
		}
		while (end > begin && IsAsciiSpace(raw[end - 1]))
		{
			--end;
		}
		const std::string host = ToLowerAscii(raw.substr(begin, end - begin));
		if (!host.empty() && !Contains(result, host))
		{
			result.push_back(host);
		}
	}
	return result;
}

std::string ValidateCreateLinkConfig(const Config& config)
{
	const std::string& base = config.apiBaseUrl;
	const bool https = StartsWithIgnoreCase(base, "https://");
	const bool http = StartsWithIgnoreCase(base, "http://");
	if (!https && !http)
	{
		return "apiBaseUrl must start with https:// (or http:// for local development)";
	}
	if (TrimTrailingSlashes(base).size() <= (https ? 8u : 7u))
	{
		return "apiBaseUrl has no host";
	}
	for (char c : base)
	{
		if (c == '?' || c == '#' || static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) == 0x7F)
		{
			return "apiBaseUrl must not contain a query, a fragment, spaces or control characters";
		}
	}
	if (config.timeoutMs <= 0)
	{
		return "timeout must be positive";
	}
	if (config.maxRetries < 0 || config.maxRetries > kMaxRetriesLimit)
	{
		return "maxRetries must be between 0 and 10";
	}
	if (config.retryDelayMs < 0)
	{
		return "retryDelay must not be negative";
	}
	return std::string();
}

std::string ValidateConfig(const Config& config)
{
	const std::string common = ValidateCreateLinkConfig(config);
	if (!common.empty())
	{
		return common;
	}
	const std::vector<std::string> hosts = NormalizeHosts(config.allowedHosts);
	if (hosts.empty())
	{
		return "allowedHosts is empty";
	}
	for (const std::string& host : hosts)
	{
		if (!IsValidHostName(host))
		{
			return "allowedHosts entry is not a plain hostname: " + host;
		}
	}
	if (config.dedupeWindowMs < 0)
	{
		return "dedupeWindow must not be negative";
	}
	if (config.parameterLinks)
	{
		return ValidateParameterLinks(*config.parameterLinks);
	}
	return std::string();
}

bool IsStrybokUrl(const std::string& url, const std::vector<std::string>& normalizedHosts)
{
	OurUrl parts;
	return ParseOurUrl(url, normalizedHosts, false, parts);
}

bool IsStrybokUrl(
	const std::string& url, const std::vector<std::string>& normalizedHosts, const ParameterLinks* parameterLinks)
{
	std::string lookup;
	return BuildLookupUrl(url, normalizedHosts, parameterLinks, lookup);
}

bool BuildLookupUrl(const std::string& url, const std::vector<std::string>& normalizedHosts, std::string& outLookup)
{
	OurUrl parts;
	if (!ParseOurUrl(url, normalizedHosts, false, parts))
	{
		return false;
	}
	// Of the query only the campaign parameters are sent; the rest and the fragment may hold
	// private data.
	outLookup = "https://" + parts.host + parts.path + BuildCampaignQuery(parts.rawQuery);
	return true;
}

std::string BuildCampaignQuery(const std::string& rawQuery)
{
	std::string values[kCampaignKeyCount];
	bool seen[kCampaignKeyCount] = {};
	std::string decoded;

	std::size_t begin = 0;
	while (begin <= rawQuery.size() && !rawQuery.empty())
	{
		std::size_t end = rawQuery.find('&', begin);
		if (end == std::string::npos)
		{
			end = rawQuery.size();
		}
		std::size_t equals = rawQuery.find('=', begin);
		const bool hasValue = equals != std::string::npos && equals < end;
		const std::size_t keyEnd = hasValue ? equals : end;

		if (DecodeUnreserved(rawQuery, begin, keyEnd, decoded))
		{
			for (std::size_t index = 0; index < kCampaignKeyCount; ++index)
			{
				if (decoded != kCampaignQueryKeys[index])
				{
					continue;
				}
				if (!seen[index])
				{
					// The first occurrence counts, even when its value is dropped.
					seen[index] = true;
					std::string value;
					if (hasValue && DecodeUnreserved(rawQuery, equals + 1, end, value) &&
						!value.empty() && value.size() <= kMaxCampaignValueLength)
					{
						values[index] = value;
					}
				}
				break;
			}
		}
		begin = end + 1;
	}

	std::string query;
	for (std::size_t index = 0; index < kCampaignKeyCount; ++index)
	{
		if (values[index].empty())
		{
			continue;
		}
		query += query.empty() ? '?' : '&';
		query += kCampaignQueryKeys[index];
		query += '=';
		query += values[index];
	}
	return query;
}

std::string PercentEncode(const std::string& value)
{
	static const char kHex[] = "0123456789ABCDEF";
	std::string result;
	result.reserve(value.size() * 3);
	for (char c : value)
	{
		if (IsAsciiAlnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
		{
			result.push_back(c);
		}
		else
		{
			const unsigned char byte = static_cast<unsigned char>(c);
			result.push_back('%');
			result.push_back(kHex[byte >> 4]);
			result.push_back(kHex[byte & 0x0F]);
		}
	}
	return result;
}

// --- links without an API call (SDK contract, "Links without an API call") ---

namespace
{

const std::size_t kMaxPayloadKeys = 20;
const std::size_t kMaxPayloadKeyLength = 40;
const std::size_t kMaxPayloadValueUnits = 256;

const char* const kReservedPayloadKeys[] = {"st", "sd", "si", "strybok_click"};

/** Never a link, and neither is anything under them. */
const char* const kReservedPathPrefixes[] = {"/.well-known", "/_strybok"};

/** Paths browsers and crawlers ask for on their own; reserved exactly. */
const char* const kReservedPathFiles[] = {"/favicon.ico", "/robots.txt", "/apple-touch-icon.png",
	"/apple-touch-icon-precomposed.png", "/sitemap.xml", "/ads.txt", "/app-ads.txt", "/security.txt", "/humans.txt",
	"/manifest.json", "/site.webmanifest", "/browserconfig.xml"};

/**
 * Whether `text` is well-formed UTF-8 (Unicode table 3-7: no overlong forms, surrogates, values
 * beyond U+10FFFF or stray continuation bytes). `outUnits` is its length in UTF-16 code units,
 * `outControl` whether it has a character in U+0000-U+001F or U+007F-U+009F.
 */
bool InspectUtf8(const std::string& text, std::size_t& outUnits, bool& outControl)
{
	outUnits = 0;
	outControl = false;
	std::size_t index = 0;
	while (index < text.size())
	{
		const unsigned char lead = static_cast<unsigned char>(text[index]);
		std::uint32_t codePoint = 0;
		std::size_t length = 0;
		if (lead < 0x80u)
		{
			codePoint = lead;
			length = 1;
		}
		else if (lead >= 0xC2u && lead <= 0xDFu)
		{
			codePoint = lead & 0x1Fu;
			length = 2;
		}
		else if (lead >= 0xE0u && lead <= 0xEFu)
		{
			codePoint = lead & 0x0Fu;
			length = 3;
		}
		else if (lead >= 0xF0u && lead <= 0xF4u)
		{
			codePoint = lead & 0x07u;
			length = 4;
		}
		else
		{
			return false;
		}
		if (text.size() - index < length)
		{
			return false;
		}
		for (std::size_t offset = 1; offset < length; ++offset)
		{
			const unsigned char next = static_cast<unsigned char>(text[index + offset]);
			if ((next & 0xC0u) != 0x80u)
			{
				return false;
			}
			codePoint = (codePoint << 6) | (next & 0x3Fu);
		}
		if ((length == 3 && codePoint < 0x800u) || (length == 4 && codePoint < 0x10000u) ||
			(codePoint >= 0xD800u && codePoint <= 0xDFFFu) || codePoint > 0x10FFFFu)
		{
			return false;
		}
		if (codePoint <= 0x1Fu || (codePoint >= 0x7Fu && codePoint <= 0x9Fu))
		{
			outControl = true;
		}
		// Counted explicitly: UTF-16 needs two code units above U+FFFF.
		outUnits += codePoint >= 0x10000u ? 2u : 1u;
		index += length;
	}
	return true;
}

/**
 * Percent-decodes `text[begin, end)` like the campaign query: `+` is a space, `%XX` a byte, the
 * bytes UTF-8. False for a malformed escape or bytes that are not UTF-8. Bytes that were not
 * escaped stand for themselves.
 */
bool DecodeComponent(const std::string& text, std::size_t begin, std::size_t end, std::string& out)
{
	out.clear();
	for (std::size_t i = begin; i < end; ++i)
	{
		const char c = text[i];
		if (c == '%')
		{
			if (end - i < 3)
			{
				return false;
			}
			const int high = HexValue(text[i + 1]);
			const int low = HexValue(text[i + 2]);
			if (high < 0 || low < 0)
			{
				return false;
			}
			out.push_back(static_cast<char>(static_cast<unsigned char>(high * 16 + low)));
			i += 2;
		}
		else
		{
			out.push_back(c == '+' ? ' ' : c);
		}
	}
	std::size_t units = 0;
	bool control = false;
	return InspectUtf8(out, units, control);
}

/** The first occurrence of a query key. */
struct QueryValue
{
	bool found = false;
	/** False when the value could not be decoded. */
	bool decoded = false;
	/** Empty for a part without `=`. */
	std::string value;
};

/**
 * The first part of `rawQuery` (split on `&`, each part on its first `=`) whose decoded key is
 * `key`, compared with case. A key that cannot be decoded is no key of ours.
 */
QueryValue FindFirst(const std::string& rawQuery, const std::string& key)
{
	QueryValue result;
	std::string decodedKey;
	std::size_t begin = 0;
	while (begin <= rawQuery.size() && !rawQuery.empty())
	{
		std::size_t end = rawQuery.find('&', begin);
		if (end == std::string::npos)
		{
			end = rawQuery.size();
		}
		// The first "=" of this part only: linear in the query, however many keys are looked up.
		std::size_t equals = begin;
		while (equals < end && rawQuery[equals] != '=')
		{
			++equals;
		}
		const bool hasValue = equals < end;
		const std::size_t keyEnd = equals;
		if (DecodeComponent(rawQuery, begin, keyEnd, decodedKey) && decodedKey == key)
		{
			result.found = true;
			result.decoded = !hasValue || DecodeComponent(rawQuery, equals + 1, end, result.value);
			if (!hasValue)
			{
				result.value.clear();
			}
			return result;
		}
		begin = end + 1;
	}
	return result;
}

/** 1-256 UTF-16 code units, none in U+0000-U+001F or U+007F-U+009F. */
bool IsPayloadValue(const std::string& value)
{
	std::size_t units = 0;
	bool control = false;
	return InspectUtf8(value, units, control) && units >= 1 && units <= kMaxPayloadValueUnits && !control;
}

/**
 * Appends `key=value` for each payload key, in the configured order, whose first occurrence in
 * `rawQuery` has a usable value, each pair preceded by `?` when it is the first of the query and
 * by `&` otherwise. Returns the number of pairs the query has now.
 */
std::size_t AppendPayloadQuery(
	std::string& out, const std::string& rawQuery, const std::vector<std::string>& payloadKeys, std::size_t pairs)
{
	for (const std::string& key : payloadKeys)
	{
		// A later occurrence never counts, even when the first one is dropped.
		const QueryValue first = FindFirst(rawQuery, key);
		if (!first.found || !first.decoded || !IsPayloadValue(first.value))
		{
			continue;
		}
		out += pairs == 0 ? '?' : '&';
		out += key;
		out += '=';
		out += PercentEncode(first.value);
		++pairs;
	}
	return pairs;
}

/** The campaign pairs after `pairs` pairs already in the query. */
void AppendCampaignQuery(std::string& out, const std::string& rawQuery, std::size_t pairs)
{
	const std::string campaign = BuildCampaignQuery(rawQuery); // "" or "?key=value&..."
	if (campaign.empty())
	{
		return;
	}
	if (pairs == 0)
	{
		out += campaign;
	}
	else
	{
		out += '&';
		out.append(campaign, 1, std::string::npos);
	}
}

bool IsDeepLinkHostChar(char c)
{
	return IsAsciiAlnum(c) || c == '.' || c == '-' || c == '_';
}

/** `A-Z a-z 0-9 - . _ ~ ! $ & ' ( ) * + , ; = : @` and `/`. */
bool IsRoutePathChar(char c)
{
	if (IsAsciiAlnum(c))
	{
		return true;
	}
	switch (c)
	{
	case '-':
	case '.':
	case '_':
	case '~':
	case '!':
	case '$':
	case '&':
	case '\'':
	case '(':
	case ')':
	case '*':
	case '+':
	case ',':
	case ';':
	case '=':
	case ':':
	case '@':
	case '/':
		return true;
	default:
		return false;
	}
}

/** Whether `path[begin, end)` is `.` or `..`, each dot possibly written `%2e` or `%2E`. */
bool IsDotSegment(const std::string& path, std::size_t begin, std::size_t end)
{
	std::size_t dots = 0;
	std::size_t i = begin;
	while (i < end)
	{
		if (path[i] == '.')
		{
			i += 1;
		}
		else if (path[i] == '%' && end - i >= 3 && path[i + 1] == '2' && (path[i + 2] == 'e' || path[i + 2] == 'E'))
		{
			i += 3;
		}
		else
		{
			return false;
		}
		++dots;
	}
	return dots == 1 || dots == 2;
}

bool IsReservedPath(const std::string& path)
{
	for (const char* prefix : kReservedPathPrefixes)
	{
		const std::string reserved(prefix);
		if (path.compare(0, reserved.size(), reserved) == 0 &&
			(path.size() == reserved.size() || path[reserved.size()] == '/'))
		{
			return true;
		}
	}
	for (const char* file : kReservedPathFiles)
	{
		if (path == file)
		{
			return true;
		}
	}
	return false;
}

/**
 * 1-512 characters starting with `/`; only route characters, every `%` followed by two
 * hexadecimal digits (the path as received, never decoded); no `//`; no `.` or `..` segment; not
 * reserved.
 */
bool IsRoutePath(const std::string& path)
{
	if (path.empty() || path.size() > kMaxRoutePathLength || path[0] != '/')
	{
		return false;
	}
	for (std::size_t i = 0; i < path.size(); ++i)
	{
		const char c = path[i];
		if (c == '%')
		{
			if (path.size() - i < 3 || HexValue(path[i + 1]) < 0 || HexValue(path[i + 2]) < 0)
			{
				return false;
			}
		}
		else if (!IsRoutePathChar(c))
		{
			return false;
		}
		if (c == '/' && i + 1 < path.size() && path[i + 1] == '/')
		{
			return false;
		}
	}
	std::size_t segmentBegin = 1;
	for (std::size_t i = 1; i <= path.size(); ++i)
	{
		if (i < path.size() && path[i] != '/')
		{
			continue;
		}
		if (IsDotSegment(path, segmentBegin, i))
		{
			return false;
		}
		segmentBegin = i + 1;
	}
	return !IsReservedPath(path);
}

/**
 * For a deep link (`http(s)://host` and an optional path, query and fragment):
 * `{lower-cased scheme}://{lower-cased host}{path}`, `/` for an empty path, followed by the payload
 * query of its own query. Its fragment and the rest of its query are dropped. False when `value`
 * is not a deep link: a character at or below U+0020, U+007F or `\`, another scheme, an empty host,
 * or anything but a path, query or fragment after the host (a port, user info, an IP literal in
 * brackets). A dotted IPv4 address passes; the server refuses it.
 */
bool RebuildDeepLink(const std::string& value, const std::vector<std::string>& payloadKeys, std::string& outDeep)
{
	for (char c : value)
	{
		const unsigned char byte = static_cast<unsigned char>(c);
		if (byte <= 0x20u || byte == 0x7Fu || c == '\\')
		{
			return false;
		}
	}
	std::string scheme;
	if (StartsWithIgnoreCase(value, "https://"))
	{
		scheme = "https";
	}
	else if (StartsWithIgnoreCase(value, "http://"))
	{
		scheme = "http";
	}
	else
	{
		return false;
	}

	const std::size_t hostBegin = scheme.size() + 3;
	std::size_t hostEnd = hostBegin;
	while (hostEnd < value.size() && IsDeepLinkHostChar(value[hostEnd]))
	{
		++hostEnd;
	}
	if (hostEnd == hostBegin)
	{
		return false;
	}
	if (hostEnd < value.size() && value[hostEnd] != '/' && value[hostEnd] != '?' && value[hostEnd] != '#')
	{
		return false;
	}

	std::size_t pathEnd = value.find_first_of("?#", hostEnd);
	if (pathEnd == std::string::npos)
	{
		pathEnd = value.size();
	}
	std::string query;
	if (pathEnd < value.size() && value[pathEnd] == '?')
	{
		std::size_t queryEnd = value.find('#', pathEnd);
		if (queryEnd == std::string::npos)
		{
			queryEnd = value.size();
		}
		query = value.substr(pathEnd + 1, queryEnd - pathEnd - 1);
	}

	outDeep = scheme + "://" + ToLowerAscii(value.substr(hostBegin, hostEnd - hostBegin));
	outDeep += pathEnd == hostEnd ? std::string("/") : value.substr(hostEnd, pathEnd - hostEnd);
	AppendPayloadQuery(outDeep, query, payloadKeys, 0);
	return true;
}

/** `https://{host}{path}`, the payload query, then the campaign query. */
std::string PathLookup(const OurUrl& parts, const std::string& path, const std::vector<std::string>& payloadKeys)
{
	std::string lookup = "https://" + parts.host + path;
	const std::size_t pairs = AppendPayloadQuery(lookup, parts.rawQuery, payloadKeys, 0);
	AppendCampaignQuery(lookup, parts.rawQuery, pairs);
	return lookup;
}

/**
 * `https://{host}/?link={deep}`, then `apn` and `ibi` when the tapped URL has them, then the
 * campaign query. Nothing else of the tapped URL is sent.
 */
bool LongLinkLookup(
	const OurUrl& parts, const QueryValue& link, const std::vector<std::string>& payloadKeys, std::string& outLookup)
{
	std::string deep;
	if (!link.decoded || !RebuildDeepLink(link.value, payloadKeys, deep))
	{
		return false;
	}
	// Sent as they are, even empty: the server compares them with the app's identifiers and
	// refuses a mismatch, as the link host does. One that cannot be decoded is not ours.
	const QueryValue apn = FindFirst(parts.rawQuery, "apn");
	const QueryValue ibi = FindFirst(parts.rawQuery, "ibi");
	if ((apn.found && !apn.decoded) || (ibi.found && !ibi.decoded))
	{
		return false;
	}
	std::string lookup = "https://" + parts.host + "/?link=" + PercentEncode(deep);
	if (apn.found)
	{
		lookup += "&apn=" + PercentEncode(apn.value);
	}
	if (ibi.found)
	{
		lookup += "&ibi=" + PercentEncode(ibi.value);
	}
	AppendCampaignQuery(lookup, parts.rawQuery, 1);
	outLookup = lookup;
	return true;
}

/** "Which URLs are ours, with the option" and "What is sent", in the order of the contract. */
bool BuildParameterLinkLookup(const OurUrl& parts, const ParameterLinks& parameterLinks, std::string& outLookup)
{
	const std::vector<std::string>& payloadKeys = parameterLinks.payloadKeys;
	// 1. A slug is ours, as without the option; the server answers it as a stored link or a path
	//    link. (Checked first: "/_strybok" is a slug, although "/_strybok/x" is reserved.)
	if (IsSlugPath(parts.path))
	{
		outLookup = PathLookup(parts, parts.path, payloadKeys);
		return true;
	}
	// 2. An empty path is the root.
	const std::string path = parts.path.empty() ? std::string("/") : parts.path;
	// 3. A Firebase long link: the root with a `link` parameter. Ours only when the link is a deep
	//    link; it is never read as a route instead.
	if (path == "/")
	{
		const QueryValue link = FindFirst(parts.rawQuery, "link");
		if (link.found)
		{
			return LongLinkLookup(parts, link, payloadKeys, outLookup);
		}
	}
	// 4. A route path.
	if (!IsRoutePath(path))
	{
		return false;
	}
	outLookup = PathLookup(parts, path, payloadKeys);
	return true;
}

} // namespace

std::string ValidateParameterLinks(const ParameterLinks& parameterLinks)
{
	const std::vector<std::string>& keys = parameterLinks.payloadKeys;
	if (keys.size() > kMaxPayloadKeys)
	{
		return "parameterLinks: at most 20 payload keys are allowed";
	}
	for (std::size_t index = 0; index < keys.size(); ++index)
	{
		const std::string& key = keys[index];
		bool shape = !key.empty() && key.size() <= kMaxPayloadKeyLength;
		for (char c : key)
		{
			shape = shape && (IsAsciiAlnum(c) || c == '_' || c == '.' || c == '-');
		}
		if (!shape)
		{
			return "parameterLinks: a payload key must be 1-40 characters of A-Z a-z 0-9 _ . -";
		}
		if (StartsWithIgnoreCase(key, "utm_"))
		{
			return "parameterLinks: \"" + key + "\" cannot be a payload key: Strybok reads utm_ keys itself";
		}
		for (const char* reserved : kReservedPayloadKeys)
		{
			if (key == reserved)
			{
				return "parameterLinks: \"" + key + "\" is reserved and cannot be a payload key";
			}
		}
		for (std::size_t other = 0; other < index; ++other)
		{
			if (keys[other] == key)
			{
				return "parameterLinks: \"" + key + "\" is listed twice";
			}
		}
	}
	return std::string();
}

bool BuildLookupUrl(
	const std::string& url,
	const std::vector<std::string>& normalizedHosts,
	const ParameterLinks* parameterLinks,
	std::string& outLookup)
{
	if (parameterLinks == nullptr)
	{
		return BuildLookupUrl(url, normalizedHosts, outLookup);
	}
	OurUrl parts;
	return ParseOurUrl(url, normalizedHosts, true, parts) && BuildParameterLinkLookup(parts, *parameterLinks, outLookup);
}

std::string BuildResolveEndpoint(const std::string& apiBaseUrl, const std::string& lookupUrl)
{
	return TrimTrailingSlashes(apiBaseUrl) + "/v1/public/resolve?url=" + PercentEncode(lookupUrl);
}

std::string BuildEventsEndpoint(const std::string& apiBaseUrl)
{
	return TrimTrailingSlashes(apiBaseUrl) + "/v1/public/events";
}

std::int64_t RetryDelayMs(std::int64_t retryDelayMs, int retryNumber)
{
	if (retryDelayMs <= 0)
	{
		return 0;
	}
	int shift = retryNumber - 1;
	if (shift < 0)
	{
		shift = 0;
	}
	if (shift > 20)
	{
		shift = 20;
	}
	const std::int64_t limit = (std::numeric_limits<std::int64_t>::max)();
	if (retryDelayMs > (limit >> shift))
	{
		return limit;
	}
	return retryDelayMs * (static_cast<std::int64_t>(1) << shift);
}

bool ParseResolveBody(const std::string& uri, const std::string& body, ResolvedLink& outLink)
{
	json::Value root;
	if (!json::Parse(body, root) || root.type != json::Type::Object)
	{
		return false;
	}
	const json::Value* linkId = root.Find("linkId");
	const json::Value* appId = root.Find("appId");
	const json::Value* routePath = root.Find("routePath");
	if (linkId == nullptr || linkId->type != json::Type::String ||
		appId == nullptr || appId->type != json::Type::String ||
		routePath == nullptr || routePath->type != json::Type::String)
	{
		return false;
	}
	// A route is an in-app path: "/x". "https://..." and "//host" could leave the app.
	const std::string& route = routePath->text;
	if (route.empty() || route[0] != '/' || (route.size() > 1 && route[1] == '/'))
	{
		return false;
	}

	outLink = ResolvedLink();
	outLink.uri = uri;
	outLink.linkId = linkId->text;
	outLink.appId = appId->text;
	outLink.routePath = route;
	const json::Value* payload = root.Find("payload");
	if (payload != nullptr && payload->type == json::Type::Object)
	{
		outLink.payloadJson = body.substr(payload->begin, payload->end - payload->begin);
	}
	const json::Value* campaign = root.Find("campaign");
	if (campaign != nullptr && campaign->type == json::Type::Object)
	{
		// Only the five string fields; a missing or odd one is simply not there.
		const auto read = [campaign](const char* key, std::string& out) {
			const json::Value* value = campaign->Find(key);
			if (value != nullptr && value->type == json::Type::String)
			{
				out = value->text;
			}
		};
		read("source", outLink.campaign.source);
		read("medium", outLink.campaign.medium);
		read("campaign", outLink.campaign.campaign);
		read("term", outLink.campaign.term);
		read("content", outLink.campaign.content);
	}
	return true;
}

AttemptResult ClassifyResolveResponse(const std::string& uri, const HttpResponse& response)
{
	AttemptResult result;
	if (!response.transportOk)
	{
		result.action = AttemptAction::Retry;
		result.error = MakeError(ErrorKind::Network, "Network error", uri);
		return result;
	}
	if (response.statusCode == 200)
	{
		if (ParseResolveBody(uri, response.body, result.link))
		{
			result.action = AttemptAction::Deliver;
		}
		else
		{
			result.action = AttemptAction::Fail;
			result.error = MakeError(ErrorKind::Server, "Malformed resolve response", uri, 200);
		}
		return result;
	}
	if (response.statusCode == 404)
	{
		result.action = AttemptAction::Fail;
		result.error = MakeError(ErrorKind::LinkNotFound, "No active link matches this URL", uri, 404);
		return result;
	}
	// Client errors will not improve; server errors might.
	result.action = response.statusCode >= 500 ? AttemptAction::Retry : AttemptAction::Fail;
	result.error = MakeError(
		ErrorKind::Server, "Unexpected status " + IntToString(response.statusCode), uri, response.statusCode);
	return result;
}

std::string FormatUuidV4(const std::uint8_t bytes[16])
{
	static const char kHex[] = "0123456789abcdef";
	std::uint8_t copy[16];
	for (int index = 0; index < 16; ++index)
	{
		copy[index] = bytes[index];
	}
	copy[6] = static_cast<std::uint8_t>((copy[6] & 0x0F) | 0x40);
	copy[8] = static_cast<std::uint8_t>((copy[8] & 0x3F) | 0x80);

	std::string result;
	result.reserve(36);
	for (int index = 0; index < 16; ++index)
	{
		if (index == 4 || index == 6 || index == 8 || index == 10)
		{
			result.push_back('-');
		}
		result.push_back(kHex[copy[index] >> 4]);
		result.push_back(kHex[copy[index] & 0x0F]);
	}
	return result;
}

std::string BuildAppOpenBody(
	const std::string& eventId,
	const std::string& linkId,
	const std::string& platform,
	const std::string& sdkVersion,
	const Campaign& campaign)
{
	const std::string version = sdkVersion.empty() ? std::string(kSdkVersion) : sdkVersion;
	std::string body = "{\"eventId\":";
	body += json::Quote(eventId);
	body += ",\"linkId\":";
	body += json::Quote(linkId);
	body += ",\"type\":\"app_open\",\"platform\":";
	body += json::Quote(platform);
	body += ",\"sdkVersion\":";
	body += json::Quote(version.substr(0, kMaxSdkVersionLength));
	if (!campaign.IsEmpty())
	{
		// What resolve returned, unchanged; an empty field is one the server did not send.
		body += ",\"campaign\":{";
		bool first = true;
		const auto append = [&body, &first](const char* key, const std::string& value) {
			if (value.empty())
			{
				return;
			}
			if (!first)
			{
				body += ",";
			}
			first = false;
			body += "\"";
			body += key;
			body += "\":";
			body += json::Quote(value);
		};
		append("source", campaign.source);
		append("medium", campaign.medium);
		append("campaign", campaign.campaign);
		append("term", campaign.term);
		append("content", campaign.content);
		body += "}";
	}
	body += "}";
	return body;
}

// --- deferred links ---

const char* const kDeferredAttemptsKey = "strybok.deferred.attempts";
const char* const kDeferredDoneKey = "strybok.deferred.done";

IDeferredLinkStore::~IDeferredLinkStore() = default;

namespace
{

const char* const kClickKey = "strybok_click";
const std::size_t kClickKeyLength = 13;

/** `^dc_[0-9A-Za-z]{32}$` */
bool IsClickId(const std::string& value)
{
	if (value.size() != 35 || value[0] != 'd' || value[1] != 'c' || value[2] != '_')
	{
		return false;
	}
	for (std::size_t index = 3; index < value.size(); ++index)
	{
		if (!IsAsciiAlnum(value[index]))
		{
			return false;
		}
	}
	return true;
}

/**
 * Looks for the first part of `text` (split on `&`, each part on its first `=`) whose key is
 * exactly `strybok_click`, compared as it stands. Returns whether such a part exists; `outClickId`
 * is filled only when its value is a click id.
 */
bool FindRawClickPart(const std::string& text, std::string& outClickId)
{
	outClickId.clear();
	std::size_t begin = 0;
	while (begin <= text.size())
	{
		std::size_t end = text.find('&', begin);
		if (end == std::string::npos)
		{
			end = text.size();
		}
		const std::size_t equals = text.find('=', begin);
		const bool hasValue = equals != std::string::npos && equals < end;
		const std::size_t keyEnd = hasValue ? equals : end;
		if (keyEnd - begin == kClickKeyLength && text.compare(begin, kClickKeyLength, kClickKey) == 0)
		{
			// The first occurrence decides, even when its value is unusable.
			if (hasValue)
			{
				const std::string value = text.substr(equals + 1, end - equals - 1);
				if (IsClickId(value))
				{
					outClickId = value;
				}
			}
			return true;
		}
		begin = end + 1;
	}
	return false;
}

bool ContainsEncodedClickKey(const std::string& text)
{
	static const char kNeedle[] = "strybok_click%3d";
	const std::size_t needleLength = sizeof(kNeedle) - 1;
	if (text.size() < needleLength)
	{
		return false;
	}
	for (std::size_t start = 0; start + needleLength <= text.size(); ++start)
	{
		std::size_t index = 0;
		while (index < needleLength && AsciiLower(text[start + index]) == kNeedle[index])
		{
			++index;
		}
		if (index == needleLength)
		{
			return true;
		}
	}
	return false;
}

/** Plain percent-decoding of the whole string (`+` stays `+`). False on a malformed escape. */
bool PercentDecodeOnce(const std::string& text, std::string& out)
{
	out.clear();
	out.reserve(text.size());
	std::size_t i = 0;
	while (i < text.size())
	{
		if (text[i] != '%')
		{
			out.push_back(text[i]);
			++i;
			continue;
		}
		if (text.size() - i < 3)
		{
			return false;
		}
		const int high = HexValue(text[i + 1]);
		const int low = HexValue(text[i + 2]);
		if (high < 0 || low < 0)
		{
			return false;
		}
		out.push_back(static_cast<char>(static_cast<unsigned char>(high * 16 + low)));
		i += 3;
	}
	return true;
}

std::string TrimAsciiSpace(const std::string& value)
{
	std::size_t begin = 0;
	std::size_t end = value.size();
	while (begin < end && IsAsciiSpace(value[begin]))
	{
		++begin;
	}
	while (end > begin && IsAsciiSpace(value[end - 1]))
	{
		--end;
	}
	return value.substr(begin, end - begin);
}

std::string DeferredSdkVersion(const std::string& sdkVersion)
{
	const std::string version = sdkVersion.empty() ? std::string(kSdkVersion) : sdkVersion;
	return version.substr(0, kMaxSdkVersionLength);
}

} // namespace

std::string BuildDeferredRedeemEndpoint(const std::string& apiBaseUrl)
{
	return TrimTrailingSlashes(apiBaseUrl) + "/v1/public/deferred-links/redeem";
}

std::string BuildDeferredMatchEndpoint(const std::string& apiBaseUrl)
{
	return TrimTrailingSlashes(apiBaseUrl) + "/v1/public/deferred-links/match";
}

bool FindClickIdInReferrer(const std::string& referrer, std::string& outClickId)
{
	if (FindRawClickPart(referrer, outClickId))
	{
		return !outClickId.empty();
	}
	// A store may hand the value back still encoded.
	std::string decoded;
	if (!ContainsEncodedClickKey(referrer) || !PercentDecodeOnce(referrer, decoded))
	{
		return false;
	}
	return FindRawClickPart(decoded, outClickId) && !outClickId.empty();
}

bool FindClickIdInPastedLink(
	const std::string& text, const std::vector<std::string>& normalizedHosts, std::string& outClickId)
{
	outClickId.clear();
	OurUrl parts;
	// The slug rule, with or without `parameterLinks`: the copied link is a stored link's.
	if (!ParseOurUrl(TrimAsciiSpace(text), normalizedHosts, false, parts))
	{
		return false;
	}
	// Standard query parsing, as for the campaign query. The key and a valid id consist of
	// unreserved characters only, so anything `DecodeUnreserved` refuses cannot be either.
	const std::string& query = parts.rawQuery;
	std::string decoded;
	std::size_t begin = 0;
	while (begin <= query.size() && !query.empty())
	{
		std::size_t end = query.find('&', begin);
		if (end == std::string::npos)
		{
			end = query.size();
		}
		const std::size_t equals = query.find('=', begin);
		const bool hasValue = equals != std::string::npos && equals < end;
		const std::size_t keyEnd = hasValue ? equals : end;
		if (DecodeUnreserved(query, begin, keyEnd, decoded) && decoded == kClickKey)
		{
			// The first occurrence decides.
			std::string value;
			if (hasValue && DecodeUnreserved(query, equals + 1, end, value) && IsClickId(value))
			{
				outClickId = value;
				return true;
			}
			return false;
		}
		begin = end + 1;
	}
	return false;
}

std::string BuildAndroidRedeemBody(
	const std::string& clickId, const std::string& packageName, const std::string& sdkVersion)
{
	std::string body = "{\"platform\":\"android\",\"clickId\":";
	body += json::Quote(clickId);
	body += ",\"packageName\":";
	body += json::Quote(packageName);
	body += ",\"sdkVersion\":";
	body += json::Quote(DeferredSdkVersion(sdkVersion));
	body += "}";
	return body;
}

std::string BuildIosRedeemBody(
	const std::string& clickId, const std::string& bundleId, const std::string& sdkVersion)
{
	std::string body = "{\"platform\":\"ios\",\"clickId\":";
	body += json::Quote(clickId);
	body += ",\"bundleId\":";
	body += json::Quote(bundleId);
	body += ",\"sdkVersion\":";
	body += json::Quote(DeferredSdkVersion(sdkVersion));
	body += "}";
	return body;
}

std::string BuildMatchBody(
	const std::string& bundleId,
	const std::string& deviceFamily,
	const std::string& locale,
	const std::string& sdkVersion)
{
	std::string body = "{\"platform\":\"ios\",\"bundleId\":";
	body += json::Quote(bundleId);
	body += ",\"deviceFamily\":";
	body += json::Quote(deviceFamily);
	body += ",\"locale\":";
	body += json::Quote(locale);
	body += ",\"sdkVersion\":";
	body += json::Quote(DeferredSdkVersion(sdkVersion));
	body += "}";
	return body;
}

bool IsMatchDeviceFamily(const std::string& value)
{
	return value == "iphone" || value == "ipad" || value == "ipod";
}

bool IsMatchLocale(const std::string& value)
{
	const auto isAlpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
	std::size_t index = 0;
	while (index < value.size() && isAlpha(value[index]))
	{
		++index;
	}
	if (index < 2 || index > 3)
	{
		return false;
	}
	int subtags = 0;
	while (index < value.size())
	{
		if (value[index] != '-' && value[index] != '_')
		{
			return false;
		}
		++index;
		const std::size_t begin = index;
		while (index < value.size() && IsAsciiAlnum(value[index]))
		{
			++index;
		}
		const std::size_t length = index - begin;
		if (length < 1 || length > 8 || ++subtags > 4)
		{
			return false;
		}
	}
	return true;
}

bool ParseDeferredBody(
	const std::string& body,
	const std::vector<std::string>& normalizedHosts,
	bool requireProbabilistic,
	ResolvedLink& outLink)
{
	json::Value root;
	if (!json::Parse(body, root) || root.type != json::Type::Object)
	{
		return false;
	}
	const json::Value* deferred = root.Find("deferred");
	const json::Value* url = root.Find("url");
	if (deferred == nullptr || deferred->type != json::Type::Bool || !deferred->boolean ||
		url == nullptr || url->type != json::Type::String || !IsStrybokUrl(url->text, normalizedHosts))
	{
		return false;
	}
	if (requireProbabilistic)
	{
		const json::Value* matched = root.Find("matched");
		if (matched == nullptr || matched->type != json::Type::String || matched->text != "probabilistic")
		{
			return false;
		}
	}
	if (!ParseResolveBody(url->text, body, outLink))
	{
		return false;
	}
	outLink.isDeferred = true;
	outLink.isProbabilisticMatch = requireProbabilistic;
	return true;
}

// --- creating links ---

std::string BuildLinksEndpoint(const std::string& apiBaseUrl)
{
	return TrimTrailingSlashes(apiBaseUrl) + "/v1/public/links";
}

std::string NormalizeClientKey(const std::string& clientKey)
{
	std::size_t begin = 0;
	std::size_t end = clientKey.size();
	while (begin < end && IsAsciiSpace(clientKey[begin]))
	{
		++begin;
	}
	while (end > begin && IsAsciiSpace(clientKey[end - 1]))
	{
		--end;
	}
	for (std::size_t index = begin; index < end; ++index)
	{
		const unsigned char byte = static_cast<unsigned char>(clientKey[index]);
		if (byte <= 0x20 || byte >= 0x7F)
		{
			return std::string(); // cannot be a header value, so it cannot be a working key
		}
	}
	return clientKey.substr(begin, end - begin);
}

bool BuildCreateLinkBody(const CreateLinkParams& params, std::string& outBody, std::string& outProblem)
{
	const std::string& route = params.routePath;
	if (route.empty() || route[0] != '/')
	{
		outProblem = "routePath must start with \"/\"";
		return false;
	}
	if (route.size() > 1 && route[1] == '/')
	{
		outProblem = "routePath must not start with \"//\"";
		return false;
	}
	// Characters as the server counts them (UTF-16 code units), from UTF-8 bytes.
	std::size_t length = 0;
	for (char c : route)
	{
		const unsigned char byte = static_cast<unsigned char>(c);
		if ((byte & 0xC0u) != 0x80u)
		{
			length += byte >= 0xF0u ? 2u : 1u;
		}
	}
	if (length > kMaxRoutePathLength)
	{
		outProblem = "routePath must be at most 512 characters";
		return false;
	}

	std::string payload;
	if (!params.payloadJson.empty())
	{
		json::Value parsed;
		if (!json::Parse(params.payloadJson, parsed) || parsed.type != json::Type::Object)
		{
			outProblem = "payload must be a JSON object";
			return false;
		}
		payload = params.payloadJson.substr(parsed.begin, parsed.end - parsed.begin);
	}

	std::string body = "{\"routePath\":";
	body += json::Quote(route);
	if (!payload.empty())
	{
		body += ",\"payload\":";
		body += payload;
	}
	if (!params.title.empty())
	{
		body += ",\"title\":";
		body += json::Quote(params.title);
	}
	if (!params.description.empty())
	{
		body += ",\"description\":";
		body += json::Quote(params.description);
	}
	if (!params.imageUrl.empty())
	{
		body += ",\"imageUrl\":";
		body += json::Quote(params.imageUrl);
	}
	body += "}";
	outBody = std::move(body);
	outProblem.clear();
	return true;
}

bool ParseCreateLinkBody(const std::string& body, CreatedLink& outLink)
{
	json::Value root;
	if (!json::Parse(body, root) || root.type != json::Type::Object)
	{
		return false;
	}
	const json::Value* linkId = root.Find("linkId");
	const json::Value* slug = root.Find("slug");
	const json::Value* routePath = root.Find("routePath");
	const json::Value* url = root.Find("url");
	if (linkId == nullptr || linkId->type != json::Type::String ||
		slug == nullptr || slug->type != json::Type::String ||
		routePath == nullptr || routePath->type != json::Type::String ||
		url == nullptr || url->type != json::Type::String)
	{
		return false;
	}
	if (url->text.compare(0, 8, "https://") != 0)
	{
		return false;
	}
	outLink = CreatedLink();
	outLink.url = url->text;
	outLink.slug = slug->text;
	outLink.routePath = routePath->text;
	outLink.linkId = linkId->text;
	return true;
}

std::int64_t ParseRetryAfterSeconds(const std::string& value)
{
	std::size_t begin = 0;
	std::size_t end = value.size();
	while (begin < end && IsAsciiSpace(value[begin]))
	{
		++begin;
	}
	while (end > begin && IsAsciiSpace(value[end - 1]))
	{
		--end;
	}
	if (begin == end)
	{
		return -1;
	}
	std::int64_t seconds = 0;
	for (std::size_t index = begin; index < end; ++index)
	{
		if (!IsAsciiDigit(value[index]))
		{
			return -1; // an HTTP date or something else; the contract only knows seconds
		}
		if (seconds < kMaxRetryAfterSeconds)
		{
			seconds = seconds * 10 + (value[index] - '0');
		}
		if (seconds > kMaxRetryAfterSeconds)
		{
			seconds = kMaxRetryAfterSeconds;
		}
	}
	return seconds;
}

namespace
{

/** The server's `error.message` and `error.details.issues`, when the body has them. */
void ReadServerValidation(const std::string& body, LinkError& error)
{
	json::Value root;
	if (!json::Parse(body, root) || root.type != json::Type::Object)
	{
		return;
	}
	const json::Value* envelope = root.Find("error");
	if (envelope == nullptr || envelope->type != json::Type::Object)
	{
		return;
	}
	const json::Value* message = envelope->Find("message");
	if (message != nullptr && message->type == json::Type::String && !message->text.empty())
	{
		error.message = message->text;
	}
	const json::Value* details = envelope->Find("details");
	const json::Value* issues =
		details != nullptr && details->type == json::Type::Object ? details->Find("issues") : nullptr;
	if (issues == nullptr || issues->type != json::Type::Array)
	{
		return;
	}
	for (const json::Value& item : issues->items)
	{
		if (item.type != json::Type::Object)
		{
			continue;
		}
		const json::Value* path = item.Find("path");
		const json::Value* text = item.Find("message");
		ValidationIssue issue;
		if (path != nullptr && path->type == json::Type::String)
		{
			issue.path = path->text;
		}
		if (text != nullptr && text->type == json::Type::String)
		{
			issue.message = text->text;
		}
		if (!issue.path.empty() || !issue.message.empty())
		{
			error.issues.push_back(std::move(issue));
		}
	}
}

} // namespace

CreateAttemptResult ClassifyCreateLinkResponse(const std::string& routePath, const HttpResponse& response)
{
	CreateAttemptResult result;
	if (!response.transportOk)
	{
		result.action = AttemptAction::Retry;
		result.error = MakeError(ErrorKind::Network, "Network error", std::string());
		return result;
	}
	const int status = response.statusCode;
	result.action = AttemptAction::Fail;
	if (status == 200 || status == 201)
	{
		// 200: an identical link already existed. 201: a new one was made. Both are success.
		if (ParseCreateLinkBody(response.body, result.link))
		{
			result.action = AttemptAction::Deliver;
		}
		else
		{
			result.error = MakeError(ErrorKind::Server, "Malformed create-link response", std::string(), status);
		}
		return result;
	}
	if (status == 401)
	{
		result.error = MakeError(
			ErrorKind::Unauthorized,
			"The client key was not accepted, or creating links from the app is switched off",
			std::string(),
			status);
		return result;
	}
	if (status == 403)
	{
		result.error = MakeError(
			ErrorKind::RouteNotAllowed,
			"The route is not covered by the application's route patterns",
			std::string(),
			status);
		result.error.routePath = routePath;
		return result;
	}
	if (status == 429)
	{
		result.error = MakeError(ErrorKind::RateLimited, "Too many requests; try again later", std::string(), status);
		result.error.retryAfterSeconds = ParseRetryAfterSeconds(response.retryAfter);
		return result;
	}
	if (status == 400 || status == 413 || status == 422)
	{
		result.error = MakeError(
			ErrorKind::Validation, "The server rejected the link (status " + IntToString(status) + ")", std::string(), status);
		ReadServerValidation(response.body, result.error);
		return result;
	}
	// Other client errors will not improve; server errors might.
	result.action = status >= 500 ? AttemptAction::Retry : AttemptAction::Fail;
	result.error = MakeError(ErrorKind::Server, "Unexpected status " + IntToString(status), std::string(), status);
	return result;
}

// ---------------------------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------------------------

namespace json
{

Value::Value() = default;
Value::Value(const Value& other) = default;
Value::Value(Value&& other) noexcept = default;
Value& Value::operator=(const Value& other) = default;
Value& Value::operator=(Value&& other) noexcept = default;
Value::~Value() = default;

const Value* Value::Find(const std::string& key) const
{
	const Value* found = nullptr;
	for (const std::pair<std::string, Value>& member : members)
	{
		if (member.first == key)
		{
			found = &member.second;
		}
	}
	return found;
}

bool Parse(const std::string& text, Value& outValue)
{
	JsonParser parser(text);
	return parser.ParseDocument(outValue);
}

std::string Quote(const std::string& value)
{
	static const char kHex[] = "0123456789abcdef";
	std::string result = "\"";
	for (char c : value)
	{
		const unsigned char byte = static_cast<unsigned char>(c);
		switch (c)
		{
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			if (byte < 0x20)
			{
				result += "\\u00";
				result.push_back(kHex[byte >> 4]);
				result.push_back(kHex[byte & 0x0F]);
			}
			else
			{
				result.push_back(c);
			}
			break;
		}
	}
	result.push_back('"');
	return result;
}

} // namespace json

// ---------------------------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------------------------

class LinkEngine::Impl : public std::enable_shared_from_this<LinkEngine::Impl>
{
public:
	Impl(Config config, IHttpTransport& transport, IScheduler& scheduler, IClock& clock, IRandom* random)
		: config_(std::move(config))
		, hosts_(NormalizeHosts(config_.allowedHosts))
		, transport_(transport)
		, scheduler_(scheduler)
		, clock_(clock)
		, random_(random)
	{
		if (config_.maxRetries < 0)
		{
			config_.maxRetries = 0;
		}
		if (config_.maxRetries > kMaxRetriesLimit)
		{
			config_.maxRetries = kMaxRetriesLimit;
		}
		if (config_.timeoutMs < 1)
		{
			config_.timeoutMs = 1;
		}
		config_.androidPackageName = TrimAsciiSpace(config_.androidPackageName);
		config_.iosBundleId = TrimAsciiSpace(config_.iosBundleId);
		sources_ = std::move(config_.installReferrerSources);
		config_.installReferrerSources.clear();
		deferredOption_ = config_.deferredLinks;
		deferredStatus_ = DeferredEffective() ? DeferredLinkStatus::Pending : DeferredLinkStatus::Disabled;
	}

	// --- deferred links: the public side ---

	DeferredLinkStatus GetDeferredLinkStatus() const { return deferredStatus_; }

	void SetDeferredLinkStatusHandler(std::function<void(DeferredLinkStatus)> handler)
	{
		statusHandler_ = std::move(handler);
	}

	void SetInstallReferrerSources(std::vector<InstallReferrerSource> sources)
	{
		if (!stopped_ && !sourcesTaken_)
		{
			sources_ = std::move(sources);
		}
	}

	void EnableDeferredLinks()
	{
		if (stopped_)
		{
			return;
		}
		deferredOption_ = true;
		if (!DeferredEffective())
		{
			return;
		}
		if (deferredStatus_ == DeferredLinkStatus::Disabled)
		{
			SetDeferredStatus(DeferredLinkStatus::Pending);
		}
		MaybeScheduleCheck();
	}

	void RedeemPastedLink(const std::string& text, std::function<void(PastedLinkResult)> callback)
	{
		if (stopped_ || !DeferredEffective() || !IsIos())
		{
			if (callback)
			{
				callback(PastedLinkResult::Disabled);
			}
			return;
		}
		PasteRequest request;
		request.text = text;
		request.done = std::move(callback);
		pasteQueue_.push_back(std::move(request));
		if (!pasteBusy_)
		{
			ProcessNextPaste();
		}
	}

	void SkipPastedLink()
	{
		if (stopped_ || !awaitingPaste_)
		{
			return;
		}
		awaitingPaste_ = false;
		ContinueMatching();
	}

	// --- configuration of the receiving side ---

	void SetLinkHandler(LinkHandler handler)
	{
		linkHandler_ = std::move(handler);
		Flush();
	}

	void SetErrorHandler(ErrorHandler handler) { errorHandler_ = std::move(handler); }

	void SetListeningProbe(std::function<bool()> probe) { listeningProbe_ = std::move(probe); }

	void SetListening(bool listening)
	{
		if (stopped_)
		{
			return;
		}
		listening_ = listening;
		Flush();
	}

	bool IsListening() const { return listening_; }
	bool IsStarted() const { return started_; }
	bool IsReady() const { return ready_; }
	bool IsStopped() const { return stopped_; }
	std::size_t PendingCount() const { return pending_.size(); }
	/** Links that arrive or are resolved; deferred links keep the slug rule (`ParseDeferredBody`). */
	bool IsOurs(const std::string& url) const { return strybok::IsStrybokUrl(url, hosts_, OptionalParameterLinks()); }

	/** The `parameterLinks` option, or null when it is not set. */
	const ParameterLinks* OptionalParameterLinks() const
	{
		return config_.parameterLinks ? &*config_.parameterLinks : nullptr;
	}

	// --- lifecycle ---

	void Start(const std::string* launchUrl, std::function<void()> onReady)
	{
		if (stopped_)
		{
			return;
		}
		if (started_)
		{
			if (onReady)
			{
				if (ready_)
				{
					onReady();
				}
				else
				{
					readyWaiters_.push_back(std::move(onReady));
				}
			}
			return;
		}
		// Accept running-app URLs first so one arriving during start-up is not missed.
		started_ = true;
		if (onReady)
		{
			readyWaiters_.push_back(std::move(onReady));
		}
		if (launchUrl != nullptr)
		{
			const std::weak_ptr<Impl> weak = weak_from_this();
			Handle(*launchUrl, true, [weak]() {
				if (const std::shared_ptr<Impl> self = weak.lock())
				{
					self->BecomeReady();
				}
			});
		}
		else
		{
			BecomeReady();
		}
	}

	void Stop()
	{
		if (stopped_)
		{
			return;
		}
		stopped_ = true;
		listening_ = false;

		std::map<std::uint64_t, std::shared_ptr<ResolveOp>> ops;
		ops.swap(ops_);
		for (const auto& entry : ops)
		{
			const ResolveOp& op = *entry.second;
			if (op.timerActive)
			{
				scheduler_.Cancel(op.timerId);
			}
			if (op.httpActive && op.httpId != 0)
			{
				transport_.Cancel(op.httpId);
			}
		}

		std::map<std::uint64_t, EventOp> events;
		events.swap(events_);
		for (const auto& entry : events)
		{
			scheduler_.Cancel(entry.second.timerId);
			if (entry.second.httpId != 0)
			{
				transport_.Cancel(entry.second.httpId);
			}
		}

		std::map<std::uint64_t, std::shared_ptr<PostOp>> posts;
		posts.swap(posts_);
		for (const auto& entry : posts)
		{
			const PostOp& op = *entry.second;
			if (op.timerActive)
			{
				scheduler_.Cancel(op.timerId);
			}
			if (op.httpActive && op.httpId != 0)
			{
				transport_.Cancel(op.httpId);
			}
		}
		if (checkTimerActive_)
		{
			checkTimerActive_ = false;
			scheduler_.Cancel(checkTimerId_);
		}
		if (askTimerActive_)
		{
			askTimerActive_ = false;
			scheduler_.Cancel(askTimerId_);
		}
		++askToken_;
		awaitingPaste_ = false;
		pasteBusy_ = false;
		pasteQueue_.clear();
		pasteDone_ = nullptr;
		sources_.clear();
		statusHandler_ = nullptr;

		pending_.clear();
		initialSeq_ = 0;
		readyWaiters_.clear();
		initialWaiters_.clear();
		linkHandler_ = nullptr;
		errorHandler_ = nullptr;
		listeningProbe_ = nullptr;
	}

	// --- incoming URLs ---

	void HandleUrl(const std::string& url)
	{
		if (!started_ || stopped_)
		{
			return;
		}
		Handle(url, false, nullptr);
	}

	void GetInitialLink(InitialLinkCallback callback)
	{
		if (!callback)
		{
			return;
		}
		if (stopped_)
		{
			callback(nullptr);
			return;
		}
		if (!ready_)
		{
			initialWaiters_.push_back(std::move(callback));
			return;
		}
		ResolvedLink link;
		const bool found = TakeInitialLink(link);
		callback(found ? &link : nullptr);
	}

	bool TakeInitialLink(ResolvedLink& outLink)
	{
		if (!ready_ || stopped_ || initialSeq_ == 0)
		{
			return false;
		}
		const std::uint64_t seq = initialSeq_;
		initialSeq_ = 0;
		for (std::size_t index = 0; index < pending_.size(); ++index)
		{
			if (pending_[index].seq == seq)
			{
				outLink = std::move(pending_[index].link);
				pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
				return true;
			}
		}
		return false; // already delivered through the link handler
	}

	bool Resolve(const std::string& url, ResolveCallback callback)
	{
		if (stopped_)
		{
			return false;
		}
		if (!IsOurs(url))
		{
			if (callback)
			{
				const LinkError error = MakeError(ErrorKind::UnsupportedLink, "Not a link for this app", url);
				callback(nullptr, &error);
			}
			return true;
		}
		StartResolve(url, std::move(callback));
		return true;
	}

private:
	struct ResolveOp
	{
		std::uint64_t id = 0;
		std::string url;
		std::string endpoint;
		/** Retries already made. */
		int retries = 0;
		/** Changes whenever the op moves on, so late callbacks of an older step are ignored. */
		std::uint64_t token = 0;
		std::uint64_t httpId = 0;
		std::uint64_t timerId = 0;
		bool httpActive = false;
		bool timerActive = false;
		ResolveCallback done;
	};

	struct EventOp
	{
		std::uint64_t httpId = 0;
		std::uint64_t timerId = 0;
	};

	struct Queued
	{
		std::uint64_t seq = 0;
		ResolvedLink link;
	};

	bool CanDeliver() const
	{
		if (!listening_ || !linkHandler_)
		{
			return false;
		}
		return !listeningProbe_ || listeningProbe_();
	}

	void Flush()
	{
		while (!stopped_ && CanDeliver() && !pending_.empty())
		{
			Queued next = std::move(pending_.front());
			pending_.erase(pending_.begin());
			if (next.seq == initialSeq_)
			{
				initialSeq_ = 0;
			}
			const LinkHandler handler = linkHandler_; // the handler may replace itself or stop us
			handler(next.link);
		}
	}

	void BecomeReady()
	{
		if (stopped_ || ready_)
		{
			return;
		}
		ready_ = true;

		std::vector<std::function<void()>> readyWaiters;
		readyWaiters.swap(readyWaiters_);
		for (const std::function<void()>& waiter : readyWaiters)
		{
			if (stopped_)
			{
				return;
			}
			waiter();
		}

		std::vector<InitialLinkCallback> initialWaiters;
		initialWaiters.swap(initialWaiters_);
		for (const InitialLinkCallback& waiter : initialWaiters)
		{
			if (stopped_)
			{
				return;
			}
			ResolvedLink link;
			const bool found = TakeInitialLink(link);
			waiter(found ? &link : nullptr);
		}

		// Last, and on a later scheduler turn: nothing of the start waits for the deferred check.
		if (!stopped_)
		{
			MaybeScheduleCheck();
		}
	}

	/** The OS can hand over one tap twice, e.g. as the launch URL and again as a running-app URL. */
	bool IsDuplicate(const std::string& url)
	{
		const std::int64_t now = clock_.NowMs();
		const bool duplicate = hasLast_ && lastUrl_ == url && (now - lastAtMs_) < config_.dedupeWindowMs;
		hasLast_ = true;
		lastUrl_ = url;
		lastAtMs_ = now;
		return duplicate;
	}

	/** The automatic flow: foreign URLs and duplicates are dropped silently. */
	void Handle(const std::string& url, bool isInitial, std::function<void()> onDone)
	{
		if (!stopped_ && IsOurs(url))
		{
			// A normal link wins over a deferred one from this moment on, whatever becomes of it.
			normalLinkAccepted_ = true;
		}
		if (stopped_ || !IsOurs(url) || IsDuplicate(url))
		{
			if (onDone && !stopped_)
			{
				onDone();
			}
			return;
		}
		const std::weak_ptr<Impl> weak = weak_from_this();
		StartResolve(url, [weak, isInitial, onDone](const ResolvedLink* link, const LinkError* error) {
			const std::shared_ptr<Impl> self = weak.lock();
			if (!self || self->stopped_)
			{
				return;
			}
			if (link != nullptr)
			{
				self->Accept(*link, isInitial);
			}
			else if (error != nullptr)
			{
				const ErrorHandler handler = self->errorHandler_;
				if (handler)
				{
					handler(*error);
				}
			}
			if (onDone && !self->stopped_)
			{
				onDone();
			}
		});
	}

	void Accept(const ResolvedLink& link, bool isInitial)
	{
		// Copied first: the handler may destroy the engine, and with it `link`.
		const std::string linkId = link.linkId;
		const Campaign campaign = link.campaign;
		if (CanDeliver())
		{
			const LinkHandler handler = linkHandler_;
			handler(link);
		}
		else
		{
			Queued queued;
			queued.seq = nextSeq_++;
			queued.link = link;
			if (isInitial)
			{
				initialSeq_ = queued.seq;
			}
			pending_.push_back(std::move(queued));
		}
		if (!stopped_ && config_.reportAppOpens)
		{
			ReportAppOpen(linkId, campaign);
		}
	}

	// --- resolving ---

	/** `url` must be ours. */
	void StartResolve(const std::string& url, ResolveCallback done)
	{
		std::string lookup;
		if (!BuildLookupUrl(url, hosts_, OptionalParameterLinks(), lookup))
		{
			return;
		}
		const std::shared_ptr<ResolveOp> op = std::make_shared<ResolveOp>();
		op->id = nextOpId_++;
		op->url = url;
		op->endpoint = BuildResolveEndpoint(config_.apiBaseUrl, lookup);
		op->done = std::move(done);
		ops_[op->id] = op;
		StartAttempt(op);
	}

	std::shared_ptr<ResolveOp> FindOp(std::uint64_t opId, std::uint64_t token) const
	{
		const auto found = ops_.find(opId);
		if (found == ops_.end() || found->second->token != token)
		{
			return nullptr;
		}
		return found->second;
	}

	void StartAttempt(const std::shared_ptr<ResolveOp>& op)
	{
		const std::weak_ptr<Impl> weak = weak_from_this();
		const std::uint64_t opId = op->id;
		const std::uint64_t token = ++op->token;

		op->timerActive = true;
		op->timerId = scheduler_.Schedule(config_.timeoutMs, [weak, opId, token]() {
			if (const std::shared_ptr<Impl> self = weak.lock())
			{
				self->OnAttemptTimeout(opId, token);
			}
		});

		HttpRequest request;
		request.method = "GET";
		request.url = op->endpoint;
		request.headers.emplace_back("Accept", "application/json");

		op->httpActive = true;
		op->httpId = 0;
		const std::uint64_t httpId = transport_.Send(request, [weak, opId, token](const HttpResponse& response) {
			if (const std::shared_ptr<Impl> self = weak.lock())
			{
				self->OnAttemptResponse(opId, token, response);
			}
		});
		// The transport may have answered before returning; then there is nothing left to cancel.
		if (op->token == token && op->httpActive)
		{
			op->httpId = httpId;
		}
	}

	void OnAttemptResponse(std::uint64_t opId, std::uint64_t token, const HttpResponse& response)
	{
		if (stopped_)
		{
			return;
		}
		const std::shared_ptr<ResolveOp> op = FindOp(opId, token);
		if (!op || !op->httpActive)
		{
			return;
		}
		op->httpActive = false;
		if (op->timerActive)
		{
			op->timerActive = false;
			scheduler_.Cancel(op->timerId);
		}

		AttemptResult result = ClassifyResolveResponse(op->url, response);
		switch (result.action)
		{
		case AttemptAction::Deliver:
			Finish(op, &result.link, nullptr);
			break;
		case AttemptAction::Fail:
			Finish(op, nullptr, &result.error);
			break;
		case AttemptAction::Retry:
			RetryOrFail(op, result.error);
			break;
		}
	}

	void OnAttemptTimeout(std::uint64_t opId, std::uint64_t token)
	{
		if (stopped_)
		{
			return;
		}
		const std::shared_ptr<ResolveOp> op = FindOp(opId, token);
		if (!op || !op->timerActive)
		{
			return;
		}
		op->timerActive = false;
		if (op->httpActive)
		{
			op->httpActive = false;
			if (op->httpId != 0)
			{
				transport_.Cancel(op->httpId);
			}
		}
		RetryOrFail(op, MakeError(ErrorKind::Timeout, "The resolve request timed out", op->url));
	}

	void RetryOrFail(const std::shared_ptr<ResolveOp>& op, const LinkError& error)
	{
		if (op->retries >= config_.maxRetries)
		{
			Finish(op, nullptr, &error);
			return;
		}
		++op->retries;
		const std::weak_ptr<Impl> weak = weak_from_this();
		const std::uint64_t opId = op->id;
		const std::uint64_t token = ++op->token;
		op->timerActive = true;
		op->timerId = scheduler_.Schedule(RetryDelayMs(config_.retryDelayMs, op->retries), [weak, opId, token]() {
			const std::shared_ptr<Impl> self = weak.lock();
			if (!self || self->stopped_)
			{
				return;
			}
			const std::shared_ptr<ResolveOp> waiting = self->FindOp(opId, token);
			if (waiting && waiting->timerActive)
			{
				waiting->timerActive = false;
				self->StartAttempt(waiting);
			}
		});
	}

	void Finish(const std::shared_ptr<ResolveOp>& op, const ResolvedLink* link, const LinkError* error)
	{
		ops_.erase(op->id);
		++op->token;
		const ResolveCallback done = std::move(op->done);
		op->done = nullptr;
		if (done)
		{
			done(link, error);
		}
	}

	// --- deferred links (SDK contract, "Deferred links"; nothing here reports an error) ---

	struct PostOp
	{
		std::uint64_t id = 0;
		HttpRequest request;
		int retries = 0;
		std::uint64_t token = 0;
		std::uint64_t httpId = 0;
		std::uint64_t timerId = 0;
		bool httpActive = false;
		bool timerActive = false;
		/** `answered` is false when the retries ran out without a usable HTTP answer. */
		std::function<void(bool answered, const HttpResponse& response)> done;
	};

	struct PasteRequest
	{
		std::string text;
		std::function<void(PastedLinkResult)> done;
	};

	bool IsAndroid() const { return config_.platform == "android"; }
	bool IsIos() const { return config_.platform == "ios"; }

	bool DeferredEffective() const
	{
		if (!deferredOption_ || !config_.deferredLinkStore)
		{
			return false;
		}
		return (IsAndroid() && !config_.androidPackageName.empty()) || (IsIos() && !config_.iosBundleId.empty());
	}

	void SetDeferredStatus(DeferredLinkStatus status)
	{
		if (deferredStatus_ == status)
		{
			return;
		}
		deferredStatus_ = status;
		const std::function<void(DeferredLinkStatus)> handler = statusHandler_; // it may stop us
		if (handler)
		{
			handler(status);
		}
	}

	bool ReadDone(bool& outDone)
	{
		std::string value;
		bool found = false;
		if (!config_.deferredLinkStore->Read(kDeferredDoneKey, value, found))
		{
			return false;
		}
		// Anything stored under the key counts: when in doubt, never deliver twice.
		outDone = found && !value.empty();
		return true;
	}

	bool ReadAttempts(int& outAttempts)
	{
		std::string value;
		bool found = false;
		if (!config_.deferredLinkStore->Read(kDeferredAttemptsKey, value, found))
		{
			return false;
		}
		outAttempts = 0;
		if (!found || value.empty())
		{
			return true;
		}
		for (char c : value)
		{
			if (!IsAsciiDigit(c) || outAttempts >= kMaxDeferredAttempts)
			{
				// Not a number we wrote, or already past the limit: treat as used up.
				outAttempts = kMaxDeferredAttempts;
				return true;
			}
			outAttempts = outAttempts * 10 + (c - '0');
		}
		return true;
	}

	/** Remembers `done` at once in memory (so the other flow sees it) and then in the store. */
	bool StoreDone()
	{
		doneMirror_ = true;
		awaitingPaste_ = false;
		return config_.deferredLinkStore->Write(kDeferredDoneKey, "1");
	}

	/** Steps "attempts is 3 or more" and "store attempts + 1". False when the check must stop. */
	bool BeginAttempt()
	{
		int attempts = 0;
		if (!ReadAttempts(attempts))
		{
			SetDeferredStatus(DeferredLinkStatus::Pending);
			return false;
		}
		if (attempts >= kMaxDeferredAttempts)
		{
			StoreDone();
			SetDeferredStatus(DeferredLinkStatus::Failed);
			return false;
		}
		// Before anything else happens: a launch that dies from here on has used up an attempt.
		if (!config_.deferredLinkStore->Write(kDeferredAttemptsKey, IntToString(attempts + 1)))
		{
			SetDeferredStatus(DeferredLinkStatus::Pending);
			return false;
		}
		return true;
	}

	void MaybeScheduleCheck()
	{
		if (stopped_ || !ready_ || checkScheduled_ || !DeferredEffective())
		{
			return;
		}
		checkScheduled_ = true; // at most once per engine
		const std::weak_ptr<Impl> weak = weak_from_this();
		checkTimerActive_ = true;
		checkTimerId_ = scheduler_.Schedule(0, [weak]() {
			const std::shared_ptr<Impl> self = weak.lock();
			if (!self || self->stopped_)
			{
				return;
			}
			self->checkTimerActive_ = false;
			self->RunCheck();
		});
	}

	void RunCheck()
	{
		if (doneMirror_)
		{
			return; // a pasted link was handled before the check began; its status stands
		}
		bool done = false;
		if (!ReadDone(done))
		{
			SetDeferredStatus(DeferredLinkStatus::Pending);
			return;
		}
		if (done)
		{
			doneMirror_ = true;
			SetDeferredStatus(DeferredLinkStatus::NotFirstLaunch);
			return;
		}
		if (IsAndroid())
		{
			SetDeferredStatus(DeferredLinkStatus::Checking);
			if (stopped_ || !BeginAttempt())
			{
				return;
			}
			sourcesTaken_ = true;
			askIndex_ = 0;
			sawReferrer_ = false;
			sawUnavailableForNow_ = false;
			AskNextSource();
			return;
		}

		// iOS: without matching nothing is automatic; only a pasted link can arrive. `done` is not
		// stored, so pasting keeps working.
		if (!config_.allowProbabilisticMatching)
		{
			SetDeferredStatus(DeferredLinkStatus::Unavailable);
			return;
		}
		SetDeferredStatus(DeferredLinkStatus::Checking);
		if (stopped_)
		{
			return;
		}
		const std::function<void(std::function<void(bool)>)> probe = config_.pasteboardMayContainLink;
		if (!probe)
		{
			ContinueMatching();
			return;
		}
		// Clipboard first: a pasted link is exact, a match is a guess.
		const std::weak_ptr<Impl> weak = weak_from_this();
		const std::uint64_t token = ++askToken_;
		askTimerActive_ = true;
		askTimerId_ = scheduler_.Schedule(kSourceTimeoutMs, [weak, token]() {
			const std::shared_ptr<Impl> self = weak.lock();
			if (self && !self->stopped_ && token == self->askToken_)
			{
				self->askTimerActive_ = false;
				self->OnPasteboardAnswer(token, false);
			}
		});
		probe([weak, token](bool mayContainLink) {
			if (const std::shared_ptr<Impl> self = weak.lock())
			{
				self->OnPasteboardAnswer(token, mayContainLink);
			}
		});
	}

	// --- Android: the stores' install referrers ---

	void AskNextSource()
	{
		// A loop, not recursion: sources may answer before returning.
		while (!stopped_)
		{
			if (askIndex_ >= sources_.size())
			{
				if (sawUnavailableForNow_)
				{
					// `done` is not stored: the next launch asks again, until the attempts run out.
					SetDeferredStatus(DeferredLinkStatus::Pending);
				}
				else
				{
					StoreDone();
					SetDeferredStatus(sawReferrer_ ? DeferredLinkStatus::NoReferrer : DeferredLinkStatus::Unavailable);
				}
				return;
			}
			const InstallReferrerSource source = sources_[askIndex_++];
			if (!source)
			{
				continue; // an empty function can never answer: "unavailable"
			}
			const std::weak_ptr<Impl> weak = weak_from_this();
			const std::uint64_t token = ++askToken_;
			askTimerActive_ = true;
			askTimerId_ = scheduler_.Schedule(kSourceTimeoutMs, [weak, token]() {
				const std::shared_ptr<Impl> self = weak.lock();
				if (self && !self->stopped_ && token == self->askToken_)
				{
					// Too slow: "unavailable for now". The call is abandoned; a late answer is ignored.
					self->askTimerActive_ = false;
					InstallReferrerAnswer late;
					late.kind = InstallReferrerAnswer::Kind::UnavailableForNow;
					self->OnSourceAnswer(token, late);
				}
			});
			askingInline_ = true;
			answeredInline_ = false;
			source([weak, token](const InstallReferrerAnswer& answer) {
				if (const std::shared_ptr<Impl> self = weak.lock())
				{
					self->OnSourceAnswer(token, answer);
				}
			});
			askingInline_ = false;
			if (!answeredInline_)
			{
				return; // the answer comes later and continues from `OnSourceAnswer`
			}
			if (redeemStarted_)
			{
				return;
			}
		}
	}

	void OnSourceAnswer(std::uint64_t token, const InstallReferrerAnswer& answer)
	{
		if (stopped_ || token != askToken_)
		{
			return;
		}
		++askToken_; // one answer per question
		if (askTimerActive_)
		{
			askTimerActive_ = false;
			scheduler_.Cancel(askTimerId_);
		}
		switch (answer.kind)
		{
		case InstallReferrerAnswer::Kind::Referrer:
		{
			sawReferrer_ = true;
			// Only the click id is taken; the rest of the referrer is neither kept nor sent.
			std::string clickId;
			if (FindClickIdInReferrer(answer.referrer, clickId))
			{
				redeemStarted_ = true;
				if (askingInline_)
				{
					answeredInline_ = true;
				}
				RedeemFromReferrer(clickId);
				return;
			}
			break;
		}
		case InstallReferrerAnswer::Kind::UnavailableForNow:
			sawUnavailableForNow_ = true;
			break;
		case InstallReferrerAnswer::Kind::Unavailable:
			break;
		}
		if (askingInline_)
		{
			answeredInline_ = true; // the loop in `AskNextSource` goes on
			return;
		}
		AskNextSource();
	}

	void RedeemFromReferrer(const std::string& clickId)
	{
		if (normalLinkAccepted_)
		{
			// A normal link wins: the id is not even sent.
			StoreDone();
			SetDeferredStatus(DeferredLinkStatus::Superseded);
			return;
		}
		const std::weak_ptr<Impl> weak = weak_from_this();
		StartPost(
			BuildDeferredRedeemEndpoint(config_.apiBaseUrl),
			BuildAndroidRedeemBody(clickId, config_.androidPackageName, config_.sdkVersion),
			[weak](bool answered, const HttpResponse& response) {
				const std::shared_ptr<Impl> self = weak.lock();
				if (self && !self->stopped_)
				{
					self->OnAutomaticAnswer(answered, response, false);
				}
			});
	}

	// --- iOS: probabilistic matching ---

	void OnPasteboardAnswer(std::uint64_t token, bool mayContainLink)
	{
		if (stopped_ || token != askToken_)
		{
			return;
		}
		++askToken_;
		if (askTimerActive_)
		{
			askTimerActive_ = false;
			scheduler_.Cancel(askTimerId_);
		}
		if (doneMirror_)
		{
			return;
		}
		if (mayContainLink)
		{
			awaitingPaste_ = true;
			SetDeferredStatus(DeferredLinkStatus::AwaitingPaste);
			return;
		}
		ContinueMatching();
	}

	void ContinueMatching()
	{
		if (stopped_ || doneMirror_)
		{
			return;
		}
		SetDeferredStatus(DeferredLinkStatus::Checking);
		if (stopped_ || !BeginAttempt())
		{
			return;
		}
		if (normalLinkAccepted_)
		{
			StoreDone();
			SetDeferredStatus(DeferredLinkStatus::Superseded);
			return;
		}
		const std::function<std::string()> familyProvider = config_.deviceFamily;
		const std::function<std::string()> localeProvider = config_.locale;
		const std::string family = familyProvider ? familyProvider() : std::string();
		const std::string locale = localeProvider ? localeProvider() : std::string();
		if (stopped_)
		{
			return;
		}
		if (!IsMatchDeviceFamily(family) || !IsMatchLocale(locale))
		{
			StoreDone();
			SetDeferredStatus(DeferredLinkStatus::Unavailable);
			return;
		}
		const std::weak_ptr<Impl> weak = weak_from_this();
		StartPost(
			BuildDeferredMatchEndpoint(config_.apiBaseUrl),
			BuildMatchBody(config_.iosBundleId, family, locale, config_.sdkVersion),
			[weak](bool answered, const HttpResponse& response) {
				const std::shared_ptr<Impl> self = weak.lock();
				if (self && !self->stopped_)
				{
					self->OnAutomaticAnswer(answered, response, true);
				}
			});
	}

	/** The answer to the automatic redeem (Android) or match (iOS) request. */
	void OnAutomaticAnswer(bool answered, const HttpResponse& response, bool probabilistic)
	{
		if (doneMirror_)
		{
			return; // a pasted link was handled meanwhile; this one is dropped
		}
		if (!answered || response.statusCode == 429)
		{
			SetDeferredStatus(DeferredLinkStatus::Pending);
			return;
		}
		if (response.statusCode == 200)
		{
			ResolvedLink link;
			if (!ParseDeferredBody(response.body, hosts_, probabilistic, link))
			{
				StoreDone();
				SetDeferredStatus(DeferredLinkStatus::Failed);
				return;
			}
			if (normalLinkAccepted_)
			{
				StoreDone();
				SetDeferredStatus(DeferredLinkStatus::Superseded);
				return;
			}
			if (!StoreDone())
			{
				// Not remembered, so delivering could happen twice: rather lose the link.
				SetDeferredStatus(DeferredLinkStatus::Failed);
				return;
			}
			SetDeferredStatus(DeferredLinkStatus::Delivered);
			DeliverDeferred(link);
			return;
		}
		StoreDone();
		SetDeferredStatus(
			response.statusCode >= 400 && response.statusCode < 500 ? DeferredLinkStatus::NotFound : DeferredLinkStatus::Failed);
	}

	/** Same queue and handler as every link; never the launch link, never an `app_open`. */
	void DeliverDeferred(const ResolvedLink& link)
	{
		if (stopped_)
		{
			return;
		}
		if (CanDeliver())
		{
			const LinkHandler handler = linkHandler_;
			handler(link);
		}
		else
		{
			Queued queued;
			queued.seq = nextSeq_++;
			queued.link = link;
			pending_.push_back(std::move(queued));
		}
	}

	// --- iOS: a pasted link ---

	void ProcessNextPaste()
	{
		while (!stopped_ && !pasteBusy_ && !pasteQueue_.empty())
		{
			PasteRequest request = std::move(pasteQueue_.front());
			pasteQueue_.erase(pasteQueue_.begin());
			pasteBusy_ = true;
			pasteDone_ = std::move(request.done);
			StartPaste(request.text);
		}
	}

	void FinishPaste(PastedLinkResult result)
	{
		const std::function<void(PastedLinkResult)> done = std::move(pasteDone_);
		pasteDone_ = nullptr;
		pasteBusy_ = false;
		if (done)
		{
			done(result);
		}
		if (!stopped_)
		{
			ProcessNextPaste();
		}
	}

	void StartPaste(const std::string& text)
	{
		if (!DeferredEffective() || !IsIos())
		{
			FinishPaste(PastedLinkResult::Disabled);
			return;
		}
		bool done = doneMirror_;
		if (!done && !ReadDone(done))
		{
			FinishPaste(PastedLinkResult::Failed);
			return;
		}
		if (done)
		{
			doneMirror_ = true;
			FinishPaste(PastedLinkResult::AlreadyHandled);
			return;
		}
		std::string clickId;
		if (!FindClickIdInPastedLink(text, hosts_, clickId))
		{
			// Whatever the person had on the clipboard: dropped here, sent nowhere.
			if (awaitingPaste_)
			{
				awaitingPaste_ = false;
				ContinueMatching();
			}
			FinishPaste(PastedLinkResult::NotOurs);
			return;
		}
		const std::weak_ptr<Impl> weak = weak_from_this();
		StartPost(
			BuildDeferredRedeemEndpoint(config_.apiBaseUrl),
			BuildIosRedeemBody(clickId, config_.iosBundleId, config_.sdkVersion),
			[weak](bool answered, const HttpResponse& response) {
				const std::shared_ptr<Impl> self = weak.lock();
				if (self && !self->stopped_)
				{
					self->OnPasteAnswer(answered, response);
				}
			});
	}

	void OnPasteAnswer(bool answered, const HttpResponse& response)
	{
		if (!answered || response.statusCode == 429)
		{
			// `done` is not stored: the person can tap Paste again.
			FinishPaste(PastedLinkResult::Failed);
			return;
		}
		if (doneMirror_)
		{
			FinishPaste(PastedLinkResult::AlreadyHandled); // the other flow finished meanwhile
			return;
		}
		if (response.statusCode == 200)
		{
			ResolvedLink link;
			if (!ParseDeferredBody(response.body, hosts_, false, link) || !StoreDone())
			{
				StoreDone();
				SetDeferredStatus(DeferredLinkStatus::Failed);
				FinishPaste(PastedLinkResult::Failed);
				return;
			}
			// Delivered even after a normal link: pasting was the person's own, explicit action.
			SetDeferredStatus(DeferredLinkStatus::Delivered);
			DeliverDeferred(link);
			FinishPaste(PastedLinkResult::Delivered);
			return;
		}
		const bool refused = response.statusCode >= 400 && response.statusCode < 500;
		StoreDone();
		SetDeferredStatus(refused ? DeferredLinkStatus::NotFound : DeferredLinkStatus::Failed);
		FinishPaste(refused ? PastedLinkResult::NotFound : PastedLinkResult::Failed);
	}

	// --- deferred requests: POST with the timeout and retries of resolving ---

	void StartPost(
		const std::string& endpoint, const std::string& body, std::function<void(bool, const HttpResponse&)> done)
	{
		const std::shared_ptr<PostOp> op = std::make_shared<PostOp>();
		op->id = nextOpId_++;
		op->request.method = "POST";
		op->request.url = endpoint;
		op->request.headers.emplace_back("Content-Type", "application/json");
		op->request.headers.emplace_back("Accept", "application/json");
		op->request.body = body; // built once: every retry sends the identical body
		op->done = std::move(done);
		posts_[op->id] = op;
		StartPostAttempt(op);
	}

	std::shared_ptr<PostOp> FindPost(std::uint64_t opId, std::uint64_t token) const
	{
		const auto found = posts_.find(opId);
		if (found == posts_.end() || found->second->token != token)
		{
			return nullptr;
		}
		return found->second;
	}

	void StartPostAttempt(const std::shared_ptr<PostOp>& op)
	{
		const std::weak_ptr<Impl> weak = weak_from_this();
		const std::uint64_t opId = op->id;
		const std::uint64_t token = ++op->token;

		op->timerActive = true;
		op->timerId = scheduler_.Schedule(config_.timeoutMs, [weak, opId, token]() {
			const std::shared_ptr<Impl> self = weak.lock();
			if (!self || self->stopped_)
			{
				return;
			}
			const std::shared_ptr<PostOp> waiting = self->FindPost(opId, token);
			if (!waiting || !waiting->timerActive)
			{
				return;
			}
			waiting->timerActive = false;
			if (waiting->httpActive)
			{
				waiting->httpActive = false;
				if (waiting->httpId != 0)
				{
					self->transport_.Cancel(waiting->httpId);
				}
			}
			self->RetryPostOrGiveUp(waiting);
		});

		op->httpActive = true;
		op->httpId = 0;
		const std::uint64_t httpId = transport_.Send(op->request, [weak, opId, token](const HttpResponse& response) {
			const std::shared_ptr<Impl> self = weak.lock();
			if (!self || self->stopped_)
			{
				return;
			}
			const std::shared_ptr<PostOp> waiting = self->FindPost(opId, token);
			if (!waiting || !waiting->httpActive)
			{
				return;
			}
			waiting->httpActive = false;
			if (waiting->timerActive)
			{
				waiting->timerActive = false;
				self->scheduler_.Cancel(waiting->timerId);
			}
			if (!response.transportOk || response.statusCode >= 500)
			{
				self->RetryPostOrGiveUp(waiting);
				return;
			}
			self->FinishPost(waiting, true, response);
		});
		if (op->token == token && op->httpActive)
		{
			op->httpId = httpId;
		}
	}

	void RetryPostOrGiveUp(const std::shared_ptr<PostOp>& op)
	{
		if (op->retries >= config_.maxRetries)
		{
			FinishPost(op, false, HttpResponse());
			return;
		}
		++op->retries;
		const std::weak_ptr<Impl> weak = weak_from_this();
		const std::uint64_t opId = op->id;
		const std::uint64_t token = ++op->token;
		op->timerActive = true;
		op->timerId = scheduler_.Schedule(RetryDelayMs(config_.retryDelayMs, op->retries), [weak, opId, token]() {
			const std::shared_ptr<Impl> self = weak.lock();
			if (!self || self->stopped_)
			{
				return;
			}
			const std::shared_ptr<PostOp> waiting = self->FindPost(opId, token);
			if (waiting && waiting->timerActive)
			{
				waiting->timerActive = false;
				self->StartPostAttempt(waiting);
			}
		});
	}

	void FinishPost(const std::shared_ptr<PostOp>& op, bool answered, const HttpResponse& response)
	{
		posts_.erase(op->id);
		++op->token;
		const std::function<void(bool, const HttpResponse&)> done = std::move(op->done);
		op->done = nullptr;
		if (done)
		{
			done(answered, response);
		}
	}

	// --- analytics ---

	/** Best effort: never awaited by delivery, every failure is swallowed. */
	void ReportAppOpen(const std::string& linkId, const Campaign& campaign)
	{
		std::uint8_t bytes[16] = {};
		if (random_ != nullptr)
		{
			random_->Fill(bytes, sizeof(bytes));
		}
		else
		{
			defaultRandom_.Fill(bytes, sizeof(bytes));
		}

		HttpRequest request;
		request.method = "POST";
		request.url = BuildEventsEndpoint(config_.apiBaseUrl);
		request.headers.emplace_back("Content-Type", "application/json");
		request.body = BuildAppOpenBody(FormatUuidV4(bytes), linkId, config_.platform, config_.sdkVersion, campaign);

		const std::weak_ptr<Impl> weak = weak_from_this();
		const std::uint64_t eventId = nextEventId_++;
		EventOp& event = events_[eventId];
		event.timerId = scheduler_.Schedule(config_.timeoutMs, [weak, eventId]() {
			const std::shared_ptr<Impl> self = weak.lock();
			if (!self || self->stopped_)
			{
				return;
			}
			const auto found = self->events_.find(eventId);
			if (found != self->events_.end())
			{
				const std::uint64_t httpId = found->second.httpId;
				self->events_.erase(found);
				if (httpId != 0)
				{
					self->transport_.Cancel(httpId);
				}
			}
		});

		const std::uint64_t httpId = transport_.Send(request, [weak, eventId](const HttpResponse&) {
			const std::shared_ptr<Impl> self = weak.lock();
			if (!self || self->stopped_)
			{
				return;
			}
			const auto found = self->events_.find(eventId);
			if (found != self->events_.end())
			{
				const std::uint64_t timerId = found->second.timerId;
				self->events_.erase(found);
				self->scheduler_.Cancel(timerId);
			}
		});
		const auto stillOpen = events_.find(eventId);
		if (stillOpen != events_.end())
		{
			stillOpen->second.httpId = httpId;
		}
	}

	Config config_;
	std::vector<std::string> hosts_;
	IHttpTransport& transport_;
	IScheduler& scheduler_;
	IClock& clock_;
	IRandom* random_;
	StdRandom defaultRandom_;

	LinkHandler linkHandler_;
	ErrorHandler errorHandler_;
	std::function<bool()> listeningProbe_;
	bool listening_ = false;
	bool started_ = false;
	bool ready_ = false;
	bool stopped_ = false;

	/** Links resolved while nobody was listening (typically the cold-start link). */
	std::vector<Queued> pending_;
	std::uint64_t nextSeq_ = 1;
	/** Queue entry of the launch link; 0 when there is none or it was handed out. */
	std::uint64_t initialSeq_ = 0;

	bool hasLast_ = false;
	std::string lastUrl_;
	std::int64_t lastAtMs_ = 0;

	std::map<std::uint64_t, std::shared_ptr<ResolveOp>> ops_;
	std::uint64_t nextOpId_ = 1;
	std::map<std::uint64_t, EventOp> events_;
	std::uint64_t nextEventId_ = 1;

	std::vector<std::function<void()>> readyWaiters_;
	std::vector<InitialLinkCallback> initialWaiters_;

	// Deferred links.
	bool deferredOption_ = false;
	DeferredLinkStatus deferredStatus_ = DeferredLinkStatus::Disabled;
	std::function<void(DeferredLinkStatus)> statusHandler_;
	std::vector<InstallReferrerSource> sources_;
	/** True once the check started asking the sources; the list is fixed from then on. */
	bool sourcesTaken_ = false;
	bool checkScheduled_ = false;
	bool checkTimerActive_ = false;
	std::uint64_t checkTimerId_ = 0;
	/** A link of this app was accepted from the OS since the engine started. */
	bool normalLinkAccepted_ = false;
	/** `done` as this process knows it: set the moment either flow decides to store it. */
	bool doneMirror_ = false;
	std::size_t askIndex_ = 0;
	bool sawReferrer_ = false;
	bool sawUnavailableForNow_ = false;
	bool redeemStarted_ = false;
	bool askingInline_ = false;
	bool answeredInline_ = false;
	/** Identifies the one question (source or pasteboard probe) whose answer is still wanted. */
	std::uint64_t askToken_ = 0;
	bool askTimerActive_ = false;
	std::uint64_t askTimerId_ = 0;
	bool awaitingPaste_ = false;
	std::vector<PasteRequest> pasteQueue_;
	bool pasteBusy_ = false;
	std::function<void(PastedLinkResult)> pasteDone_;
	std::map<std::uint64_t, std::shared_ptr<PostOp>> posts_;
};

LinkEngine::LinkEngine(Config config, IHttpTransport& transport, IScheduler& scheduler, IClock& clock, IRandom* random)
	: impl_(std::make_shared<Impl>(std::move(config), transport, scheduler, clock, random))
{
}

LinkEngine::~LinkEngine()
{
	impl_->Stop();
}

void LinkEngine::SetLinkHandler(LinkHandler handler)
{
	const std::shared_ptr<Impl> keepAlive = impl_;
	keepAlive->SetLinkHandler(std::move(handler));
}

void LinkEngine::SetErrorHandler(ErrorHandler handler)
{
	impl_->SetErrorHandler(std::move(handler));
}

void LinkEngine::SetListeningProbe(std::function<bool()> probe)
{
	impl_->SetListeningProbe(std::move(probe));
}

void LinkEngine::SetListening(bool listening)
{
	const std::shared_ptr<Impl> keepAlive = impl_;
	keepAlive->SetListening(listening);
}

bool LinkEngine::IsListening() const
{
	return impl_->IsListening();
}

void LinkEngine::Start(const std::string* launchUrl, std::function<void()> onReady)
{
	const std::shared_ptr<Impl> keepAlive = impl_;
	keepAlive->Start(launchUrl, std::move(onReady));
}

bool LinkEngine::IsStarted() const
{
	return impl_->IsStarted();
}

bool LinkEngine::IsReady() const
{
	return impl_->IsReady();
}

bool LinkEngine::IsStopped() const
{
	return impl_->IsStopped();
}

void LinkEngine::HandleUrl(const std::string& url)
{
	const std::shared_ptr<Impl> keepAlive = impl_;
	keepAlive->HandleUrl(url);
}

void LinkEngine::GetInitialLink(InitialLinkCallback callback)
{
	const std::shared_ptr<Impl> keepAlive = impl_;
	keepAlive->GetInitialLink(std::move(callback));
}

bool LinkEngine::TakeInitialLink(ResolvedLink& outLink)
{
	return impl_->TakeInitialLink(outLink);
}

bool LinkEngine::Resolve(const std::string& url, ResolveCallback callback)
{
	const std::shared_ptr<Impl> keepAlive = impl_;
	return keepAlive->Resolve(url, std::move(callback));
}

bool LinkEngine::IsStrybokUrl(const std::string& url) const
{
	return impl_->IsOurs(url);
}

std::size_t LinkEngine::PendingCount() const
{
	return impl_->PendingCount();
}

DeferredLinkStatus LinkEngine::GetDeferredLinkStatus() const
{
	return impl_->GetDeferredLinkStatus();
}

void LinkEngine::SetDeferredLinkStatusHandler(std::function<void(DeferredLinkStatus)> handler)
{
	impl_->SetDeferredLinkStatusHandler(std::move(handler));
}

void LinkEngine::EnableDeferredLinks()
{
	const std::shared_ptr<Impl> keepAlive = impl_;
	keepAlive->EnableDeferredLinks();
}

void LinkEngine::SetInstallReferrerSources(std::vector<InstallReferrerSource> sources)
{
	impl_->SetInstallReferrerSources(std::move(sources));
}

void LinkEngine::RedeemPastedLink(const std::string& text, std::function<void(PastedLinkResult)> callback)
{
	const std::shared_ptr<Impl> keepAlive = impl_;
	keepAlive->RedeemPastedLink(text, std::move(callback));
}

void LinkEngine::SkipPastedLink()
{
	const std::shared_ptr<Impl> keepAlive = impl_;
	keepAlive->SkipPastedLink();
}

void LinkEngine::Stop()
{
	const std::shared_ptr<Impl> keepAlive = impl_;
	keepAlive->Stop();
}

// ---------------------------------------------------------------------------------------------
// Creating links
// ---------------------------------------------------------------------------------------------

class LinkCreator::Impl : public std::enable_shared_from_this<LinkCreator::Impl>
{
public:
	Impl(Config config, IHttpTransport& transport, IScheduler& scheduler)
		: config_(std::move(config))
		, clientKey_(NormalizeClientKey(config_.clientKey))
		, endpoint_(BuildLinksEndpoint(config_.apiBaseUrl))
		, transport_(transport)
		, scheduler_(scheduler)
	{
		if (config_.maxRetries < 0)
		{
			config_.maxRetries = 0;
		}
		if (config_.maxRetries > kMaxRetriesLimit)
		{
			config_.maxRetries = kMaxRetriesLimit;
		}
		if (config_.timeoutMs < 1)
		{
			config_.timeoutMs = 1;
		}
	}

	bool IsStopped() const { return stopped_; }
	std::size_t InFlightCount() const { return ops_.size(); }

	bool Create(const CreateLinkParams& params, Callback callback)
	{
		if (stopped_)
		{
			return false;
		}
		// The checks of the contract, in its order, before any network call.
		if (clientKey_.empty())
		{
			if (callback)
			{
				const LinkError error = MakeError(ErrorKind::Unauthorized, "No client key is configured", std::string());
				callback(nullptr, &error);
			}
			return true;
		}
		std::string body;
		std::string problem;
		if (!BuildCreateLinkBody(params, body, problem))
		{
			if (callback)
			{
				const LinkError error = MakeError(ErrorKind::Validation, problem, std::string());
				callback(nullptr, &error);
			}
			return true;
		}

		const std::shared_ptr<CreateOp> op = std::make_shared<CreateOp>();
		op->id = nextOpId_++;
		op->routePath = params.routePath;
		op->body = std::move(body);
		op->done = std::move(callback);
		ops_[op->id] = op;
		StartAttempt(op);
		return true;
	}

	void Stop()
	{
		if (stopped_)
		{
			return;
		}
		stopped_ = true;
		std::map<std::uint64_t, std::shared_ptr<CreateOp>> ops;
		ops.swap(ops_);
		for (const auto& entry : ops)
		{
			const CreateOp& op = *entry.second;
			if (op.timerActive)
			{
				scheduler_.Cancel(op.timerId);
			}
			if (op.httpActive && op.httpId != 0)
			{
				transport_.Cancel(op.httpId);
			}
		}
	}

private:
	struct CreateOp
	{
		std::uint64_t id = 0;
		std::string routePath;
		/** Built once: every retry sends exactly the same bytes. */
		std::string body;
		/** Retries already made. */
		int retries = 0;
		/** Changes whenever the op moves on, so late callbacks of an older step are ignored. */
		std::uint64_t token = 0;
		std::uint64_t httpId = 0;
		std::uint64_t timerId = 0;
		bool httpActive = false;
		bool timerActive = false;
		Callback done;
	};

	std::shared_ptr<CreateOp> FindOp(std::uint64_t opId, std::uint64_t token) const
	{
		const auto found = ops_.find(opId);
		if (found == ops_.end() || found->second->token != token)
		{
			return nullptr;
		}
		return found->second;
	}

	void StartAttempt(const std::shared_ptr<CreateOp>& op)
	{
		const std::weak_ptr<Impl> weak = weak_from_this();
		const std::uint64_t opId = op->id;
		const std::uint64_t token = ++op->token;

		op->timerActive = true;
		op->timerId = scheduler_.Schedule(config_.timeoutMs, [weak, opId, token]() {
			if (const std::shared_ptr<Impl> self = weak.lock())
			{
				self->OnAttemptTimeout(opId, token);
			}
		});

		HttpRequest request;
		request.method = "POST";
		request.url = endpoint_;
		request.headers.emplace_back("Content-Type", "application/json");
		request.headers.emplace_back("Accept", "application/json");
		// Not `Authorization`: the key is publishable and must never be taken for a bearer secret.
		request.headers.emplace_back("X-Strybok-Key", clientKey_);
		request.body = op->body;

		op->httpActive = true;
		op->httpId = 0;
		const std::uint64_t httpId = transport_.Send(request, [weak, opId, token](const HttpResponse& response) {
			if (const std::shared_ptr<Impl> self = weak.lock())
			{
				self->OnAttemptResponse(opId, token, response);
			}
		});
		// The transport may have answered before returning; then there is nothing left to cancel.
		if (op->token == token && op->httpActive)
		{
			op->httpId = httpId;
		}
	}

	void OnAttemptResponse(std::uint64_t opId, std::uint64_t token, const HttpResponse& response)
	{
		if (stopped_)
		{
			return;
		}
		const std::shared_ptr<CreateOp> op = FindOp(opId, token);
		if (!op || !op->httpActive)
		{
			return;
		}
		op->httpActive = false;
		if (op->timerActive)
		{
			op->timerActive = false;
			scheduler_.Cancel(op->timerId);
		}

		const CreateAttemptResult result = ClassifyCreateLinkResponse(op->routePath, response);
		switch (result.action)
		{
		case AttemptAction::Deliver:
			Finish(op, &result.link, nullptr);
			break;
		case AttemptAction::Fail:
			Finish(op, nullptr, &result.error);
			break;
		case AttemptAction::Retry:
			RetryOrFail(op, result.error);
			break;
		}
	}

	void OnAttemptTimeout(std::uint64_t opId, std::uint64_t token)
	{
		if (stopped_)
		{
			return;
		}
		const std::shared_ptr<CreateOp> op = FindOp(opId, token);
		if (!op || !op->timerActive)
		{
			return;
		}
		op->timerActive = false;
		if (op->httpActive)
		{
			op->httpActive = false;
			if (op->httpId != 0)
			{
				transport_.Cancel(op->httpId);
			}
		}
		RetryOrFail(op, MakeError(ErrorKind::Timeout, "The create-link request timed out", std::string()));
	}

	void RetryOrFail(const std::shared_ptr<CreateOp>& op, const LinkError& error)
	{
		if (op->retries >= config_.maxRetries)
		{
			Finish(op, nullptr, &error);
			return;
		}
		++op->retries;
		const std::weak_ptr<Impl> weak = weak_from_this();
		const std::uint64_t opId = op->id;
		const std::uint64_t token = ++op->token;
		op->timerActive = true;
		op->timerId = scheduler_.Schedule(RetryDelayMs(config_.retryDelayMs, op->retries), [weak, opId, token]() {
			const std::shared_ptr<Impl> self = weak.lock();
			if (!self || self->stopped_)
			{
				return;
			}
			const std::shared_ptr<CreateOp> waiting = self->FindOp(opId, token);
			if (waiting && waiting->timerActive)
			{
				waiting->timerActive = false;
				self->StartAttempt(waiting);
			}
		});
	}

	/** Nothing of this object is touched after the callback: it may stop or destroy the creator. */
	void Finish(const std::shared_ptr<CreateOp>& op, const CreatedLink* link, const LinkError* error)
	{
		ops_.erase(op->id);
		++op->token;
		const Callback done = std::move(op->done);
		op->done = nullptr;
		if (done)
		{
			done(link, error);
		}
	}

	Config config_;
	std::string clientKey_;
	std::string endpoint_;
	IHttpTransport& transport_;
	IScheduler& scheduler_;
	bool stopped_ = false;
	std::map<std::uint64_t, std::shared_ptr<CreateOp>> ops_;
	std::uint64_t nextOpId_ = 1;
};

LinkCreator::LinkCreator(Config config, IHttpTransport& transport, IScheduler& scheduler)
	: impl_(std::make_shared<Impl>(std::move(config), transport, scheduler))
{
}

LinkCreator::~LinkCreator()
{
	impl_->Stop();
}

bool LinkCreator::Create(const CreateLinkParams& params, Callback callback)
{
	const std::shared_ptr<Impl> keepAlive = impl_;
	return keepAlive->Create(params, std::move(callback));
}

std::size_t LinkCreator::InFlightCount() const
{
	return impl_->InFlightCount();
}

bool LinkCreator::IsStopped() const
{
	return impl_->IsStopped();
}

void LinkCreator::Stop()
{
	const std::shared_ptr<Impl> keepAlive = impl_;
	keepAlive->Stop();
}

} // namespace strybok
