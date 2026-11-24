// Standard library only: see the notes at the top of StrybokCore.h.

#include "StrybokCore.h"

#include <limits>
#include <map>
#include <random>

namespace strybok
{

const char* const kSdkVersion = "0.2.0";

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
};

/** The single place that decides whether a URL is ours. */
bool ParseOurUrl(const std::string& url, const std::vector<std::string>& normalizedHosts, OurUrl& outParts)
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

	// Exactly one slug segment; query and fragment are ignored.
	std::size_t pathEnd = url.find_first_of("?#", authorityEnd);
	if (pathEnd == std::string::npos)
	{
		pathEnd = url.size();
	}
	const std::string path = url.substr(authorityEnd, pathEnd - authorityEnd);
	if (!IsSlugPath(path))
	{
		return false;
	}

	outParts.host = host;
	outParts.path = path;
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
	return std::string();
}

bool IsStrybokUrl(const std::string& url, const std::vector<std::string>& normalizedHosts)
{
	OurUrl parts;
	return ParseOurUrl(url, normalizedHosts, parts);
}

bool BuildLookupUrl(const std::string& url, const std::vector<std::string>& normalizedHosts, std::string& outLookup)
{
	OurUrl parts;
	if (!ParseOurUrl(url, normalizedHosts, parts))
	{
		return false;
	}
	outLookup = "https://" + parts.host + parts.path;
	return true;
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
	const std::string& sdkVersion)
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
	body += "}";
	return body;
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
	bool IsOurs(const std::string& url) const { return strybok::IsStrybokUrl(url, hosts_); }

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
		const std::string linkId = link.linkId;
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
			ReportAppOpen(linkId);
		}
	}

	// --- resolving ---

	/** `url` must be ours. */
	void StartResolve(const std::string& url, ResolveCallback done)
	{
		std::string lookup;
		if (!BuildLookupUrl(url, hosts_, lookup))
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

	// --- analytics ---

	/** Best effort: never awaited by delivery, every failure is swallowed. */
	void ReportAppOpen(const std::string& linkId)
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
		request.body = BuildAppOpenBody(FormatUuidV4(bytes), linkId, config_.platform, config_.sdkVersion);

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
