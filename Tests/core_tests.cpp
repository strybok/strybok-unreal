// Standalone tests of the engine-independent core (Source/Strybok/Private/StrybokCore).
//
// No Unreal Engine and no test framework needed: run Tests/run_core_tests.sh.
// The cases marked [ref] are ports of the Flutter SDK's test suite
// (github.com/strybok/strybok-flutter), the reference test list of
// docs/sdk-contract.md. Time is virtual (FakeLoop), so the
// suite is deterministic and takes milliseconds.

#include "StrybokCore.h"

#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace strybok;

// ---------------------------------------------------------------------------------------------
// Tiny test runner
// ---------------------------------------------------------------------------------------------

namespace
{

struct TestCase
{
	const char* group;
	const char* name;
	void (*run)();
};

std::vector<TestCase>& Registry()
{
	static std::vector<TestCase> cases;
	return cases;
}

struct Registrar
{
	Registrar(const char* group, const char* name, void (*run)())
	{
		Registry().push_back(TestCase{group, name, run});
	}
};

int gChecks = 0;
int gFailuresInTest = 0;

[[maybe_unused]] std::string ToText(const std::string& value) { return "\"" + value + "\""; }
[[maybe_unused]] std::string ToText(const char* value) { return std::string("\"") + value + "\""; }
[[maybe_unused]] std::string ToText(bool value) { return value ? "true" : "false"; }
[[maybe_unused]] std::string ToText(int value) { return std::to_string(value); }
[[maybe_unused]] std::string ToText(long value) { return std::to_string(value); }
[[maybe_unused]] std::string ToText(long long value) { return std::to_string(value); }
[[maybe_unused]] std::string ToText(unsigned long value) { return std::to_string(value); }
[[maybe_unused]] std::string ToText(unsigned long long value) { return std::to_string(value); }
[[maybe_unused]] std::string ToText(ErrorKind value) { return "ErrorKind#" + std::to_string(static_cast<int>(value)); }

void ReportFailure(const char* file, int line, const std::string& what)
{
	++gFailuresInTest;
	std::printf("    FAILED %s:%d: %s\n", file, line, what.c_str());
}

template <typename A, typename B>
void ExpectEq(const A& actual, const B& expected, const char* actualText, const char* file, int line)
{
	++gChecks;
	if (!(actual == expected))
	{
		ReportFailure(file, line, std::string(actualText) + " is " + ToText(actual) + ", expected " + ToText(expected));
	}
}

void ExpectTrue(bool condition, const char* text, const char* file, int line, const std::string& reason)
{
	++gChecks;
	if (!condition)
	{
		ReportFailure(file, line, std::string(text) + (reason.empty() ? "" : " [" + reason + "]"));
	}
}

} // namespace

#define TEST(group, name)                                              \
	static void Test_##group##_##name();                               \
	static Registrar registrar_##group##_##name(#group, #name, &Test_##group##_##name); \
	static void Test_##group##_##name()

#define EXPECT(condition) ExpectTrue((condition), #condition, __FILE__, __LINE__, std::string())
#define EXPECT_MSG(condition, reason) ExpectTrue((condition), #condition, __FILE__, __LINE__, (reason))
#define EXPECT_EQ(actual, expected) ExpectEq((actual), (expected), #actual, __FILE__, __LINE__)

// ---------------------------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------------------------

namespace
{

const std::string kHost = "my-app.strybok.link";
const std::string kApi = "https://api.strybok.com";
const std::string kInvite = "https://my-app.strybok.link/invite777";
const std::string kResolvePrefix = "https://api.strybok.com/v1/public/resolve?url=";

/** Virtual time: a clock and a scheduler that only move when the test says so. */
class FakeLoop final : public IScheduler, public IClock
{
public:
	std::uint64_t Schedule(std::int64_t delayMs, std::function<void()> task) override
	{
		Task entry;
		entry.at = now_ + (delayMs > 0 ? delayMs : 0);
		entry.id = nextId_++;
		entry.run = std::move(task);
		tasks_.push_back(std::move(entry));
		scheduledDelays.push_back(delayMs);
		return tasks_.back().id;
	}

	void Cancel(std::uint64_t timerId) override
	{
		for (std::size_t index = 0; index < tasks_.size(); ++index)
		{
			if (tasks_[index].id == timerId)
			{
				tasks_.erase(tasks_.begin() + static_cast<std::ptrdiff_t>(index));
				return;
			}
		}
	}

	std::int64_t NowMs() override { return now_; }

	/** Runs everything due within the next `ms` milliseconds, in order, then lands on that time. */
	void Advance(std::int64_t ms)
	{
		const std::int64_t target = now_ + ms;
		for (;;)
		{
			std::size_t best = tasks_.size();
			for (std::size_t index = 0; index < tasks_.size(); ++index)
			{
				if (tasks_[index].at > target)
				{
					continue;
				}
				if (best == tasks_.size() || tasks_[index].at < tasks_[best].at ||
					(tasks_[index].at == tasks_[best].at && tasks_[index].id < tasks_[best].id))
				{
					best = index;
				}
			}
			if (best == tasks_.size())
			{
				break;
			}
			Task task = std::move(tasks_[best]);
			tasks_.erase(tasks_.begin() + static_cast<std::ptrdiff_t>(best));
			if (task.at > now_)
			{
				now_ = task.at;
			}
			task.run();
		}
		now_ = target;
	}

	/** The Dart tests' `settle()`: long enough for answers, shorter than the 60 ms timeout. */
	void Settle() { Advance(20); }

	std::size_t TaskCount() const { return tasks_.size(); }

	std::vector<std::int64_t> scheduledDelays;

private:
	struct Task
	{
		std::int64_t at = 0;
		std::uint64_t id = 0;
		std::function<void()> run;
	};

	std::int64_t now_ = 1000000;
	std::uint64_t nextId_ = 1;
	std::vector<Task> tasks_;
};

struct Reply
{
	enum class Kind
	{
		Respond,
		NetworkError,
		/** Never answers on its own; the test may call `FakeApi::Complete`. */
		Hang,
	};

	Kind kind = Kind::Respond;
	int status = 200;
	std::string body;
	/** The `Retry-After` response header; empty for none. */
	std::string retryAfter;
};

Reply Respond(int status, const std::string& body = std::string())
{
	Reply reply;
	reply.status = status;
	reply.body = body;
	return reply;
}

Reply NetworkError()
{
	Reply reply;
	reply.kind = Reply::Kind::NetworkError;
	return reply;
}

Reply Hang()
{
	Reply reply;
	reply.kind = Reply::Kind::Hang;
	return reply;
}

std::string OkBody(const std::string& route = "/invite/abc123", const std::string& linkId = "link_1")
{
	return "{\"linkId\":\"" + linkId + "\",\"appId\":\"app_1\",\"routePath\":\"" + route +
		"\",\"payload\":{\"invitationId\":\"abc123\"}}";
}

Reply Ok(const std::string& route = "/invite/abc123", const std::string& linkId = "link_1")
{
	return Respond(200, OkBody(route, linkId));
}

std::string CreatedBody(const std::string& route = "/product/42")
{
	return "{\"linkId\":\"link_9\",\"url\":\"https://my-app.strybok.link/Ab3xK9\",\"slug\":\"Ab3xK9\",\"routePath\":\"" +
		route + "\"}";
}

HttpResponse ToResponse(const Reply& reply)
{
	HttpResponse response;
	response.transportOk = reply.kind == Reply::Kind::Respond;
	response.statusCode = response.transportOk ? reply.status : 0;
	response.body = reply.body;
	response.retryAfter = reply.retryAfter;
	return response;
}

/**
 * Records requests and answers resolve calls with `onResolve`, event calls with `onEvent` and
 * create-link calls with `onCreate`.
 */
class FakeApi final : public IHttpTransport
{
public:
	explicit FakeApi(FakeLoop& loop) : loop_(loop) {}

	std::function<Reply(const HttpRequest&)> onResolve;
	std::function<Reply()> onEvent;
	std::function<Reply(const HttpRequest&)> onCreate;
	/** Answer inside `Send` instead of on the next loop turn. */
	bool synchronous = false;

	std::vector<HttpRequest> resolves;
	std::vector<HttpRequest> events;
	std::vector<std::uint64_t> resolveIds;
	std::vector<HttpRequest> creates;
	std::vector<std::uint64_t> createIds;
	std::vector<std::uint64_t> cancelled;

	std::uint64_t Send(const HttpRequest& request, Callback onDone) override
	{
		const std::uint64_t id = nextId_++;
		const std::string eventsSuffix = "/v1/public/events";
		const bool isEvent = request.url.size() >= eventsSuffix.size() &&
			request.url.compare(request.url.size() - eventsSuffix.size(), eventsSuffix.size(), eventsSuffix) == 0;
		const std::string linksSuffix = "/v1/public/links";
		const bool isCreate = request.url.size() >= linksSuffix.size() &&
			request.url.compare(request.url.size() - linksSuffix.size(), linksSuffix.size(), linksSuffix) == 0;
		Reply reply;
		if (isEvent)
		{
			events.push_back(request);
			reply = onEvent ? onEvent() : Respond(202);
		}
		else if (isCreate)
		{
			creates.push_back(request);
			createIds.push_back(id);
			reply = onCreate ? onCreate(request) : Respond(201, CreatedBody());
		}
		else
		{
			resolves.push_back(request);
			resolveIds.push_back(id);
			reply = onResolve ? onResolve(request) : Ok();
		}

		if (reply.kind == Reply::Kind::Hang)
		{
			held_[id] = std::move(onDone);
		}
		else if (synchronous)
		{
			onDone(ToResponse(reply));
		}
		else
		{
			const HttpResponse response = ToResponse(reply);
			loop_.Schedule(0, [this, id, onDone, response]() {
				if (cancelledSet_.count(id) == 0)
				{
					onDone(response);
				}
			});
		}
		return id;
	}

	void Cancel(std::uint64_t requestId) override
	{
		cancelled.push_back(requestId);
		cancelledSet_.insert(requestId);
	}

	/**
	 * Answers a hanging request now, even when it was cancelled: a real transport might still
	 * call back late, and the engine has to ignore that.
	 */
	bool Complete(std::uint64_t requestId, const Reply& reply)
	{
		const auto found = held_.find(requestId);
		if (found == held_.end())
		{
			return false;
		}
		const Callback callback = std::move(found->second);
		held_.erase(found);
		callback(ToResponse(reply));
		return true;
	}

private:
	FakeLoop& loop_;
	std::uint64_t nextId_ = 1;
	std::map<std::uint64_t, Callback> held_;
	std::set<std::uint64_t> cancelledSet_;
};

class FixedRandom final : public IRandom
{
public:
	void Fill(std::uint8_t* bytes, std::size_t count) override
	{
		for (std::size_t index = 0; index < count; ++index)
		{
			bytes[index] = 0xFF;
		}
	}
};

struct Options
{
	int maxRetries = 2;
	std::int64_t timeoutMs = 60;
	std::int64_t retryDelayMs = 1;
	bool reportAppOpens = true;
	std::string apiBaseUrl = kApi;
};

/** The Dart tests' `bridge(...)`: an engine wired to fakes, recording links and errors. */
struct Bridge
{
	FakeLoop loop;
	FakeApi api{loop};
	std::vector<ResolvedLink> received;
	std::vector<LinkError> errors;
	std::unique_ptr<LinkEngine> sdk;

	explicit Bridge(const Options& options = Options())
	{
		Config config;
		config.apiBaseUrl = options.apiBaseUrl;
		config.allowedHosts = {kHost};
		config.timeoutMs = options.timeoutMs;
		config.maxRetries = options.maxRetries;
		config.retryDelayMs = options.retryDelayMs;
		config.reportAppOpens = options.reportAppOpens;
		config.platform = "ios";
		sdk.reset(new LinkEngine(config, api, loop, loop));
		sdk->SetErrorHandler([this](const LinkError& error) { errors.push_back(error); });
	}

	/** `sdk.links.listen(received.add)` */
	void Listen()
	{
		sdk->SetLinkHandler([this](const ResolvedLink& link) { received.push_back(link); });
		sdk->SetListening(true);
	}

	/** `await sdk.initialize()` with a launch URL (empty = none). Returns whether it became ready. */
	bool Initialize(const std::string& launchUrl = std::string())
	{
		bool ready = false;
		sdk->Start(launchUrl.empty() ? nullptr : &launchUrl, [&ready]() { ready = true; });
		loop.Settle();
		return ready;
	}

	struct ResolveOutcome
	{
		bool called = false;
		bool ok = false;
		ResolvedLink link;
		LinkError error;
	};

	/** `await sdk.resolveUri(url)`, giving it `waitMs` of virtual time. */
	ResolveOutcome Resolve(const std::string& url, std::int64_t waitMs = 1000)
	{
		ResolveOutcome outcome;
		sdk->Resolve(url, [&outcome](const ResolvedLink* link, const LinkError* error) {
			outcome.called = true;
			outcome.ok = link != nullptr;
			if (link != nullptr)
			{
				outcome.link = *link;
			}
			if (error != nullptr)
			{
				outcome.error = *error;
			}
		});
		loop.Advance(waitMs);
		return outcome;
	}

	struct InitialOutcome
	{
		bool called = false;
		bool hasLink = false;
		ResolvedLink link;
	};

	InitialOutcome GetInitialLink()
	{
		InitialOutcome outcome;
		sdk->GetInitialLink([&outcome](const ResolvedLink* link) {
			outcome.called = true;
			outcome.hasLink = link != nullptr;
			if (link != nullptr)
			{
				outcome.link = *link;
			}
		});
		loop.Settle();
		return outcome;
	}
};

std::vector<std::string> Hosts()
{
	return NormalizeHosts({"My-App.Strybok.Link"});
}

std::string StringMember(const json::Value& object, const std::string& key)
{
	const json::Value* member = object.Find(key);
	return (member != nullptr && member->type == json::Type::String) ? member->text : std::string("<missing>");
}

bool IsUuidV4(const std::string& text)
{
	if (text.size() != 36)
	{
		return false;
	}
	for (std::size_t index = 0; index < text.size(); ++index)
	{
		const char c = text[index];
		if (index == 8 || index == 13 || index == 18 || index == 23)
		{
			if (c != '-')
			{
				return false;
			}
		}
		else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
		{
			return false;
		}
	}
	return text[14] == '4' && (text[19] == '8' || text[19] == '9' || text[19] == 'a' || text[19] == 'b');
}

} // namespace

// ---------------------------------------------------------------------------------------------
// host allowlist and parsing
// ---------------------------------------------------------------------------------------------

TEST(Allowlist, AcceptsHttpsLinksOnAnAllowedHostWithASingleSlug) // [ref]
{
	const std::vector<std::string> hosts = Hosts();
	EXPECT(IsStrybokUrl(kInvite, hosts));
	EXPECT(IsStrybokUrl("https://MY-APP.strybok.link/AbC_7-x", hosts));
	EXPECT(IsStrybokUrl("https://my-app.strybok.link/abc?utm=1#frag", hosts));
}

TEST(Allowlist, RejectsEverythingElse) // [ref]
{
	const std::vector<std::string> hosts = Hosts();
	const std::vector<std::string> urls = {
		"http://my-app.strybok.link/invite777",
		"myapp://my-app.strybok.link/invite777",
		"https://evil.example.com/invite777",
		"https://my-app.strybok.link.evil.example.com/invite777",
		"https://evil-my-app.strybok.link/invite777",
		"https://user:pw@my-app.strybok.link/invite777",
		"https://my-app.strybok.link:8443/invite777",
		"https://my-app.strybok.link/",
		"https://my-app.strybok.link/a/b",
		"https://my-app.strybok.link/.well-known/assetlinks.json",
		"https://my-app.strybok.link/a%2Fb",
	};
	for (const std::string& url : urls)
	{
		EXPECT_MSG(!IsStrybokUrl(url, hosts), url);
	}
}

TEST(Allowlist, RejectsMalformedAndTrickyUrls)
{
	const std::vector<std::string> hosts = Hosts();
	const std::vector<std::string> urls = {
		"",
		"https://",
		"https:///invite777",
		"my-app.strybok.link/invite777",
		" https://my-app.strybok.link/invite777",
		"https://my-app.strybok.link",
		"https://my-app.strybok.link?x=1",
		"https://my-app.strybok.link/invite777/",
		"https://my-app.strybok.link//invite777",
		"https://my-app.strybok.link/invite777\n",
		"https://my-app.strybok.link/inv ite",
		"https://my-app.strybok.link/caf\xC3\xA9",
		"https://my-app.strybok.link\\@evil.example.com/invite777",
		"https://evil.example.com\\my-app.strybok.link/invite777",
		"https://@my-app.strybok.link/invite777",
		"https://my-app.strybok.link@evil.example.com/invite777",
		"https://evil.example.com/x?https://my-app.strybok.link/invite777",
		"https://evil.example.com#https://my-app.strybok.link/invite777",
		"https://my-app.strybok.link:/invite777",
		"https://my-app.strybok.link:80/invite777",
		"https://my-app.strybok.link:443x/invite777",
		"https://my-app.strybok.link:4430000/invite777",
		"https://my-app.strybok.link./invite777",
		"https://my-app.strybok.link/" + std::string(65, 'a'),
	};
	for (const std::string& url : urls)
	{
		EXPECT_MSG(!IsStrybokUrl(url, hosts), url);
	}
}

TEST(Allowlist, AcceptsPort443UpperCaseSchemeAndAMaximumLengthSlug)
{
	const std::vector<std::string> hosts = Hosts();
	EXPECT(IsStrybokUrl("https://my-app.strybok.link:443/invite777", hosts));
	EXPECT(IsStrybokUrl("HTTPS://my-app.strybok.link/invite777", hosts));
	EXPECT(IsStrybokUrl("https://my-app.strybok.link/" + std::string(64, 'a'), hosts));
	EXPECT(IsStrybokUrl("https://my-app.strybok.link/a", hosts));
	EXPECT(IsStrybokUrl("https://my-app.strybok.link/abc?next=https://evil.example.com/x", hosts));
}

TEST(Allowlist, HostsAreNormalisedOnce)
{
	const std::vector<std::string> hosts = NormalizeHosts({"  My-App.Strybok.Link ", "", "my-app.strybok.link", "Other.Strybok.Link"});
	EXPECT_EQ(hosts.size(), static_cast<std::size_t>(2));
	EXPECT_EQ(hosts[0], "my-app.strybok.link");
	EXPECT_EQ(hosts[1], "other.strybok.link");
}

TEST(Allowlist, ResolveRefusesForeignLinksWithoutAnyNetworkCall) // [ref]
{
	Bridge bridge;
	const Bridge::ResolveOutcome outcome = bridge.Resolve("https://evil.example.com/invite777");
	EXPECT(outcome.called);
	EXPECT(!outcome.ok);
	EXPECT_EQ(outcome.error.kind, ErrorKind::UnsupportedLink);
	EXPECT_EQ(outcome.error.url, "https://evil.example.com/invite777");
	EXPECT(bridge.api.resolves.empty());
}

// ---------------------------------------------------------------------------------------------
// request building
// ---------------------------------------------------------------------------------------------

TEST(Requests, LookupUrlHasOnlyLowerCasedHostAndSlug)
{
	const std::vector<std::string> hosts = Hosts();
	std::string lookup;
	EXPECT(BuildLookupUrl("HTTPS://My-App.Strybok.Link:443/Invite777?token=secret#frag", hosts, lookup));
	EXPECT_EQ(lookup, "https://my-app.strybok.link/Invite777");
	EXPECT(!BuildLookupUrl("https://evil.example.com/invite777", hosts, lookup));
}

TEST(Requests, PercentEncodeKeepsOnlyUnreservedCharacters)
{
	EXPECT_EQ(PercentEncode("https://a-b.c/x_y~z"), "https%3A%2F%2Fa-b.c%2Fx_y~z");
	EXPECT_EQ(PercentEncode("a b&c=d?e#f%"), "a%20b%26c%3Dd%3Fe%23f%25");
	EXPECT_EQ(PercentEncode("\xC3\xA9"), "%C3%A9");
}

TEST(Requests, EndpointsDropTrailingSlashesOfTheBase)
{
	EXPECT_EQ(BuildResolveEndpoint("https://api.strybok.com/", "https://h/x"),
		"https://api.strybok.com/v1/public/resolve?url=https%3A%2F%2Fh%2Fx");
	EXPECT_EQ(BuildEventsEndpoint("https://example.com/strybok//"), "https://example.com/strybok/v1/public/events");
}

TEST(Requests, RetryDelayDoublesAndNeverOverflows)
{
	EXPECT_EQ(RetryDelayMs(400, 1), static_cast<std::int64_t>(400));
	EXPECT_EQ(RetryDelayMs(400, 2), static_cast<std::int64_t>(800));
	EXPECT_EQ(RetryDelayMs(400, 3), static_cast<std::int64_t>(1600));
	EXPECT_EQ(RetryDelayMs(0, 3), static_cast<std::int64_t>(0));
	EXPECT(RetryDelayMs(INT64_MAX / 2, 10) > 0);
	EXPECT(RetryDelayMs(400, 1000) > 0);
}

TEST(Requests, ConfigValidation)
{
	Config config;
	config.apiBaseUrl = "https://api.strybok.com";
	config.allowedHosts = {"my-app.strybok.link"};
	EXPECT_EQ(ValidateConfig(config), "");

	Config local = config;
	local.apiBaseUrl = "http://localhost:8787/prefix/";
	EXPECT_EQ(ValidateConfig(local), "");

	Config bad = config;
	bad.apiBaseUrl = "";
	EXPECT(!ValidateConfig(bad).empty());
	bad.apiBaseUrl = "api.strybok.com";
	EXPECT(!ValidateConfig(bad).empty());
	bad.apiBaseUrl = "https://";
	EXPECT(!ValidateConfig(bad).empty());
	bad.apiBaseUrl = "https://api.strybok.com?x=1";
	EXPECT(!ValidateConfig(bad).empty());

	bad = config;
	bad.allowedHosts = {" "};
	EXPECT(!ValidateConfig(bad).empty());
	bad.allowedHosts = {"https://my-app.strybok.link"};
	EXPECT(!ValidateConfig(bad).empty());
	bad.allowedHosts = {"my-app.strybok.link/path"};
	EXPECT(!ValidateConfig(bad).empty());

	bad = config;
	bad.timeoutMs = 0;
	EXPECT(!ValidateConfig(bad).empty());
	bad = config;
	bad.maxRetries = -1;
	EXPECT(!ValidateConfig(bad).empty());
	bad = config;
	bad.retryDelayMs = -1;
	EXPECT(!ValidateConfig(bad).empty());
	bad = config;
	bad.dedupeWindowMs = -1;
	EXPECT(!ValidateConfig(bad).empty());
}

// ---------------------------------------------------------------------------------------------
// JSON reader
// ---------------------------------------------------------------------------------------------

TEST(Json, ParsesValuesOfEveryType)
{
	json::Value root;
	EXPECT(json::Parse(" {\"s\":\"a\",\"n\":-12.5e+3,\"t\":true,\"f\":false,\"z\":null,\"a\":[1,[2],{}],\"o\":{\"k\":\"v\"}} ", root));
	EXPECT(root.type == json::Type::Object);
	EXPECT_EQ(root.members.size(), static_cast<std::size_t>(7));
	EXPECT_EQ(StringMember(root, "s"), "a");
	EXPECT(root.Find("n")->type == json::Type::Number);
	EXPECT_EQ(root.Find("n")->text, "-12.5e+3");
	EXPECT(root.Find("t")->type == json::Type::Bool && root.Find("t")->boolean);
	EXPECT(root.Find("f")->type == json::Type::Bool && !root.Find("f")->boolean);
	EXPECT(root.Find("z")->type == json::Type::Null);
	EXPECT_EQ(root.Find("a")->items.size(), static_cast<std::size_t>(3));
	EXPECT_EQ(StringMember(*root.Find("o"), "k"), "v");
	EXPECT(root.Find("missing") == nullptr);
}

TEST(Json, DecodesEscapesAndSurrogatePairs)
{
	json::Value value;
	EXPECT(json::Parse("\"a\\\"b\\\\c\\/d\\n\\t\\u0041\\u00e9\\ud83d\\ude00\"", value));
	EXPECT_EQ(value.text, "a\"b\\c/d\n\tA\xC3\xA9\xF0\x9F\x98\x80");
	EXPECT(json::Parse("\"\\ud83dx\"", value));
	EXPECT_EQ(value.text, "\xEF\xBF\xBDx");
	EXPECT(json::Parse("\"\\ud83d\\u0041\"", value));
	EXPECT_EQ(value.text, "\xEF\xBF\xBD" "A");
}

TEST(Json, LastDuplicateKeyWins)
{
	json::Value root;
	EXPECT(json::Parse("{\"k\":\"first\",\"k\":\"second\"}", root));
	EXPECT_EQ(StringMember(root, "k"), "second");
}

TEST(Json, RejectsInvalidDocuments)
{
	const std::vector<std::string> documents = {
		"",
		"   ",
		"not json",
		"{",
		"{\"a\":1,}",
		"{\"a\" 1}",
		"{a:1}",
		"{'a':1}",
		"[1,]",
		"[1 2]",
		"01",
		"1.",
		"-",
		"1e",
		"+1",
		"tru",
		"nul",
		"\"unterminated",
		"\"bad \\x escape\"",
		"\"bad \\u12 escape\"",
		"\"raw \n newline\"",
		"{} trailing",
		"{}{}",
	};
	for (const std::string& document : documents)
	{
		json::Value value;
		EXPECT_MSG(!json::Parse(document, value), document);
	}
}

TEST(Json, RejectsExcessiveNesting)
{
	json::Value value;
	EXPECT(json::Parse(std::string(40, '[') + std::string(40, ']'), value));
	EXPECT(!json::Parse(std::string(500, '[') + std::string(500, ']'), value));
	EXPECT(!json::Parse(std::string(100000, '['), value));
}

TEST(Json, QuoteEscapesWhatJsonRequires)
{
	EXPECT_EQ(json::Quote("plain"), "\"plain\"");
	EXPECT_EQ(json::Quote("a\"b\\c\n\x01"), "\"a\\\"b\\\\c\\n\\u0001\"");
	json::Value value;
	EXPECT(json::Parse(json::Quote("round \"trip\" \\ \t \x1F \xC3\xA9"), value));
	EXPECT_EQ(value.text, "round \"trip\" \\ \t \x1F \xC3\xA9");
}

// ---------------------------------------------------------------------------------------------
// response validation
// ---------------------------------------------------------------------------------------------

TEST(Responses, PayloadIsTheJsonObjectOrEmpty)
{
	ResolvedLink link;
	EXPECT(ParseResolveBody(kInvite, "{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"/x\",\"payload\": {\"a\": [1, {\"b\": \"}\"}]} }", link));
	EXPECT_EQ(link.payloadJson, "{\"a\": [1, {\"b\": \"}\"}]}");
	EXPECT_EQ(link.uri, kInvite);

	EXPECT(ParseResolveBody(kInvite, "{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"/x\"}", link));
	EXPECT_EQ(link.payloadJson, "{}");
	EXPECT(ParseResolveBody(kInvite, "{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"/x\",\"payload\":null}", link));
	EXPECT_EQ(link.payloadJson, "{}");
	EXPECT(ParseResolveBody(kInvite, "{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"/x\",\"payload\":[1,2]}", link));
	EXPECT_EQ(link.payloadJson, "{}");
	EXPECT(ParseResolveBody(kInvite, "{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"/x\",\"payload\":\"text\"}", link));
	EXPECT_EQ(link.payloadJson, "{}");
}

TEST(Responses, RouteMustBeAnInAppPath)
{
	ResolvedLink link;
	EXPECT(ParseResolveBody(kInvite, "{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"/\"}", link));
	EXPECT(ParseResolveBody(kInvite, "{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"/a//b\",\"extra\":1}", link));
	EXPECT(!ParseResolveBody(kInvite, "{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"\"}", link));
	EXPECT(!ParseResolveBody(kInvite, "{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"x\"}", link));
	EXPECT(!ParseResolveBody(kInvite, "{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"//\"}", link));
	EXPECT(!ParseResolveBody(kInvite, "{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":null}", link));
	EXPECT(!ParseResolveBody(kInvite, "{\"linkId\":\"l\",\"appId\":7,\"routePath\":\"/x\"}", link));
}

TEST(Responses, ClassificationTable)
{
	HttpResponse response;
	response.transportOk = true;

	response.statusCode = 200;
	response.body = OkBody();
	EXPECT(ClassifyResolveResponse(kInvite, response).action == AttemptAction::Deliver);

	response.body = "{}";
	AttemptResult result = ClassifyResolveResponse(kInvite, response);
	EXPECT(result.action == AttemptAction::Fail);
	EXPECT_EQ(result.error.kind, ErrorKind::Server);
	EXPECT_EQ(result.error.statusCode, 200);

	response.statusCode = 404;
	result = ClassifyResolveResponse(kInvite, response);
	EXPECT(result.action == AttemptAction::Fail);
	EXPECT_EQ(result.error.kind, ErrorKind::LinkNotFound);

	for (int status : {204, 301, 400, 401, 403, 410, 422, 429, 499})
	{
		response.statusCode = status;
		result = ClassifyResolveResponse(kInvite, response);
		EXPECT_MSG(result.action == AttemptAction::Fail, std::to_string(status));
		EXPECT_EQ(result.error.kind, ErrorKind::Server);
		EXPECT_EQ(result.error.statusCode, status);
		EXPECT_EQ(result.error.url, kInvite);
	}
	for (int status : {500, 502, 503, 504, 599})
	{
		response.statusCode = status;
		result = ClassifyResolveResponse(kInvite, response);
		EXPECT_MSG(result.action == AttemptAction::Retry, std::to_string(status));
		EXPECT_EQ(result.error.kind, ErrorKind::Server);
		EXPECT_EQ(result.error.statusCode, status);
	}

	HttpResponse offline;
	result = ClassifyResolveResponse(kInvite, offline);
	EXPECT(result.action == AttemptAction::Retry);
	EXPECT_EQ(result.error.kind, ErrorKind::Network);
	EXPECT_EQ(result.error.url, kInvite);
}

// ---------------------------------------------------------------------------------------------
// resolveUri
// ---------------------------------------------------------------------------------------------

TEST(Resolve, ReturnsRouteAndPayloadAndSendsOnlyHostAndSlugToTheApi) // [ref]
{
	Bridge bridge;
	const std::string url = "https://my-app.strybok.link/invite777?token=secret#f";
	const Bridge::ResolveOutcome outcome = bridge.Resolve(url);

	EXPECT(outcome.ok);
	EXPECT_EQ(outcome.link.routePath, "/invite/abc123");
	EXPECT_EQ(outcome.link.payloadJson, "{\"invitationId\":\"abc123\"}");
	EXPECT_EQ(outcome.link.linkId, "link_1");
	EXPECT_EQ(outcome.link.appId, "app_1");
	EXPECT_EQ(outcome.link.uri, url);

	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(1));
	const HttpRequest& request = bridge.api.resolves[0];
	EXPECT_EQ(request.method, "GET");
	EXPECT_EQ(request.url, kResolvePrefix + "https%3A%2F%2Fmy-app.strybok.link%2Finvite777");
	EXPECT(request.url.find("secret") == std::string::npos);
	EXPECT(request.body.empty());
	EXPECT_EQ(request.headers.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(request.headers[0].first, "Accept");
	EXPECT_EQ(request.headers[0].second, "application/json");
	EXPECT_MSG(bridge.api.events.empty(), "explicit resolution is not an app open");
	EXPECT(bridge.received.empty());
	EXPECT_EQ(bridge.sdk->PendingCount(), static_cast<std::size_t>(0));
}

TEST(Resolve, KeepsAPathPrefixOfTheApiBaseUrl) // [ref]
{
	Options options;
	options.apiBaseUrl = "https://example.com/strybok/";
	Bridge bridge(options);
	EXPECT(bridge.Resolve(kInvite).ok);
	EXPECT_EQ(bridge.api.resolves[0].url,
		"https://example.com/strybok/v1/public/resolve?url=https%3A%2F%2Fmy-app.strybok.link%2Finvite777");
}

TEST(Resolve, NotFoundBecomesLinkNotFoundAndIsNotRetried) // [ref]
{
	Bridge bridge;
	bridge.api.onResolve = [](const HttpRequest&) { return Respond(404, "{\"error\":{\"code\":\"LINK_NOT_FOUND\"}}"); };
	const Bridge::ResolveOutcome outcome = bridge.Resolve(kInvite);
	EXPECT(outcome.called && !outcome.ok);
	EXPECT_EQ(outcome.error.kind, ErrorKind::LinkNotFound);
	EXPECT_EQ(outcome.error.url, kInvite);
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(1));
}

TEST(Resolve, OtherClientErrorsAreNotRetried) // [ref]
{
	Bridge bridge;
	bridge.api.onResolve = [](const HttpRequest&) { return Respond(422, "{}"); };
	const Bridge::ResolveOutcome outcome = bridge.Resolve(kInvite);
	EXPECT(outcome.called && !outcome.ok);
	EXPECT_EQ(outcome.error.kind, ErrorKind::Server);
	EXPECT_EQ(outcome.error.statusCode, 422);
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(1));
}

TEST(Resolve, RetriesNetworkErrorsAndThenSucceeds) // [ref]
{
	Bridge bridge;
	int calls = 0;
	bridge.api.onResolve = [&calls](const HttpRequest&) {
		++calls;
		return calls < 3 ? NetworkError() : Ok();
	};
	const Bridge::ResolveOutcome outcome = bridge.Resolve(kInvite);
	EXPECT(outcome.ok);
	EXPECT_EQ(outcome.link.routePath, "/invite/abc123");
	EXPECT_EQ(calls, 3);
}

TEST(Resolve, GivesUpWithANetworkErrorAfterMaxRetries) // [ref]
{
	Options options;
	options.maxRetries = 1;
	Bridge bridge(options);
	bridge.api.onResolve = [](const HttpRequest&) { return NetworkError(); };
	const Bridge::ResolveOutcome outcome = bridge.Resolve(kInvite);
	EXPECT(outcome.called && !outcome.ok);
	EXPECT_EQ(outcome.error.kind, ErrorKind::Network);
	EXPECT_EQ(outcome.error.url, kInvite);
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(2));
}

TEST(Resolve, RetriesServerErrors) // [ref]
{
	Bridge bridge;
	int calls = 0;
	bridge.api.onResolve = [&calls](const HttpRequest&) { return ++calls == 1 ? Respond(503) : Ok(); };
	EXPECT(bridge.Resolve(kInvite).ok);
	EXPECT_EQ(calls, 2);
}

TEST(Resolve, ReportsTheLastServerErrorWhenRetriesRunOut)
{
	Bridge bridge;
	int calls = 0;
	bridge.api.onResolve = [&calls](const HttpRequest&) { return Respond(500 + (++calls)); };
	const Bridge::ResolveOutcome outcome = bridge.Resolve(kInvite);
	EXPECT(outcome.called && !outcome.ok);
	EXPECT_EQ(outcome.error.kind, ErrorKind::Server);
	EXPECT_EQ(outcome.error.statusCode, 503);
	EXPECT_EQ(calls, 3);
}

TEST(Resolve, TimesOutWithATypedError) // [ref]
{
	Options options;
	options.maxRetries = 1;
	Bridge bridge(options);
	bridge.api.onResolve = [](const HttpRequest&) { return Hang(); };
	const Bridge::ResolveOutcome outcome = bridge.Resolve(kInvite);
	EXPECT(outcome.called && !outcome.ok);
	EXPECT_EQ(outcome.error.kind, ErrorKind::Timeout);
	EXPECT_EQ(outcome.error.url, kInvite);
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(2));
	// Each abandoned attempt is cancelled at the transport.
	EXPECT_EQ(bridge.api.cancelled.size(), static_cast<std::size_t>(2));
	EXPECT_EQ(bridge.loop.TaskCount(), static_cast<std::size_t>(0));
}

TEST(Resolve, RejectsMalformedOrUnsafeResponses) // [ref]
{
	const std::vector<std::string> bodies = {
		"not json",
		"[]",
		"{\"linkId\":\"l\",\"appId\":\"a\"}",
		"{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"https://evil.example.com\"}",
		"{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"//evil.example.com\"}",
		"{\"linkId\":1,\"appId\":\"a\",\"routePath\":\"/x\"}",
	};
	for (const std::string& body : bodies)
	{
		Bridge bridge;
		bridge.api.onResolve = [body](const HttpRequest&) { return Respond(200, body); };
		const Bridge::ResolveOutcome outcome = bridge.Resolve(kInvite);
		EXPECT_MSG(outcome.called && !outcome.ok, body);
		EXPECT_EQ(outcome.error.kind, ErrorKind::Server);
		EXPECT_EQ(outcome.error.statusCode, 200);
		EXPECT_MSG(bridge.api.resolves.size() == 1, "a malformed 200 is not retried: " + body);
	}
}

TEST(Resolve, WaitsRetryDelayThenDoubleBeforeEachRetry)
{
	Options options;
	options.retryDelayMs = 400;
	options.timeoutMs = 5000;
	Bridge bridge(options);
	bridge.api.onResolve = [](const HttpRequest&) { return NetworkError(); };

	Bridge::ResolveOutcome outcome;
	bridge.sdk->Resolve(kInvite, [&outcome](const ResolvedLink*, const LinkError* error) {
		outcome.called = true;
		outcome.error = *error;
	});
	bridge.loop.Advance(399);
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(1));
	bridge.loop.Advance(1);
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(2));
	bridge.loop.Advance(799);
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(2));
	EXPECT(!outcome.called);
	bridge.loop.Advance(1);
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(3));
	bridge.loop.Advance(1);
	EXPECT(outcome.called);
	EXPECT_EQ(outcome.error.kind, ErrorKind::Network);
	EXPECT_EQ(bridge.loop.TaskCount(), static_cast<std::size_t>(0));
}

TEST(Resolve, ALateAnswerAfterATimeoutIsIgnored)
{
	Options options;
	options.maxRetries = 1;
	Bridge bridge(options);
	int calls = 0;
	bridge.api.onResolve = [&calls](const HttpRequest&) { return ++calls == 1 ? Hang() : Ok("/second"); };

	int callbacks = 0;
	std::string route;
	bridge.sdk->Resolve(kInvite, [&callbacks, &route](const ResolvedLink* link, const LinkError*) {
		++callbacks;
		route = link != nullptr ? link->routePath : "<error>";
	});
	bridge.loop.Advance(1000);
	EXPECT_EQ(callbacks, 1);
	EXPECT_EQ(route, "/second");

	// The first attempt answers long after it was abandoned.
	EXPECT(bridge.api.Complete(bridge.api.resolveIds[0], Ok("/first")));
	bridge.loop.Advance(1000);
	EXPECT_EQ(callbacks, 1);
	EXPECT_EQ(route, "/second");
}

TEST(Resolve, WorksWithATransportThatAnswersSynchronously)
{
	Bridge bridge;
	bridge.api.synchronous = true;
	int calls = 0;
	bridge.api.onResolve = [&calls](const HttpRequest&) { return ++calls == 1 ? Respond(503) : Ok(); };
	const Bridge::ResolveOutcome outcome = bridge.Resolve(kInvite);
	EXPECT(outcome.ok);
	EXPECT_EQ(calls, 2);
	EXPECT(bridge.api.cancelled.empty());
	EXPECT_EQ(bridge.loop.TaskCount(), static_cast<std::size_t>(0));
}

TEST(Resolve, ConcurrentResolvesAreIndependent)
{
	Bridge bridge;
	bridge.api.onResolve = [](const HttpRequest& request) {
		return request.url.find("slow") != std::string::npos ? Respond(404) : Ok("/fast");
	};
	std::vector<std::string> results;
	bridge.sdk->Resolve("https://my-app.strybok.link/slow", [&results](const ResolvedLink* link, const LinkError*) {
		results.push_back(link != nullptr ? link->routePath : "error");
	});
	bridge.sdk->Resolve("https://my-app.strybok.link/fast", [&results](const ResolvedLink* link, const LinkError*) {
		results.push_back(link != nullptr ? link->routePath : "error");
	});
	bridge.loop.Settle();
	EXPECT_EQ(results.size(), static_cast<std::size_t>(2));
	EXPECT_EQ(results[0], "error");
	EXPECT_EQ(results[1], "/fast");
}

// ---------------------------------------------------------------------------------------------
// cold start
// ---------------------------------------------------------------------------------------------

TEST(ColdStart, DeliversTheLaunchLinkOnceEvenIfTheListenerAttachesLate) // [ref]
{
	Bridge bridge;
	EXPECT(bridge.Initialize(kInvite));
	EXPECT_EQ(bridge.sdk->PendingCount(), static_cast<std::size_t>(1));

	bridge.Listen();
	bridge.loop.Settle();

	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(bridge.received[0].routePath, "/invite/abc123");
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(bridge.sdk->PendingCount(), static_cast<std::size_t>(0));
}

TEST(ColdStart, DeDuplicatesALaunchLinkThatIsReplayedOnTheStream) // [ref]
{
	Bridge bridge;
	bridge.Listen();
	bridge.sdk->Start(&kInvite);
	bridge.sdk->HandleUrl(kInvite); // the platform reports the launch URL again
	bridge.loop.Settle();

	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(bridge.api.events.size(), static_cast<std::size_t>(1));
}

TEST(ColdStart, GetInitialLinkConsumesTheLaunchLinkSoTheStreamDoesNotRepeatIt) // [ref]
{
	Bridge bridge;
	bridge.sdk->Start(&kInvite);
	const Bridge::InitialOutcome initial = bridge.GetInitialLink();
	EXPECT(initial.called && initial.hasLink);
	EXPECT_EQ(initial.link.routePath, "/invite/abc123");

	const Bridge::InitialOutcome again = bridge.GetInitialLink();
	EXPECT(again.called && !again.hasLink);

	bridge.Listen();
	bridge.loop.Settle();
	EXPECT(bridge.received.empty());
}

TEST(ColdStart, GetInitialLinkReturnsNothingOnceTheStreamAlreadyDeliveredIt) // [ref]
{
	Bridge bridge;
	bridge.Listen();
	EXPECT(bridge.Initialize(kInvite));
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(1));
	const Bridge::InitialOutcome initial = bridge.GetInitialLink();
	EXPECT(initial.called && !initial.hasLink);
}

TEST(ColdStart, NoLaunchLinkForeignLaunchLinkAndRepeatedInitializeAreHarmless) // [ref]
{
	{
		Bridge bridge;
		const std::string foreign = "https://other.example.com/x";
		int ready = 0;
		bridge.sdk->Start(&foreign, [&ready]() { ++ready; });
		bridge.sdk->Start(&foreign, [&ready]() { ++ready; });
		bridge.loop.Settle();
		EXPECT_EQ(ready, 2);
		const Bridge::InitialOutcome initial = bridge.GetInitialLink();
		EXPECT(initial.called && !initial.hasLink);
		EXPECT(bridge.api.resolves.empty());
		EXPECT(bridge.errors.empty());
	}
	{
		Bridge bridge;
		EXPECT(bridge.Initialize());
		EXPECT(bridge.sdk->IsReady());
		EXPECT(!bridge.GetInitialLink().hasLink);
		EXPECT(bridge.api.resolves.empty());
	}
}

TEST(ColdStart, ASecondStartNeverHandlesAnotherLaunchUrl)
{
	Bridge bridge;
	bridge.Listen();
	const std::string other = "https://my-app.strybok.link/other";
	int ready = 0;
	bridge.sdk->Start(&kInvite, [&ready]() { ++ready; });
	bridge.sdk->Start(&other, [&ready]() { ++ready; });
	EXPECT_EQ(ready, 0);
	bridge.loop.Settle();
	EXPECT_EQ(ready, 2);
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(1));

	bridge.sdk->Start(nullptr, [&ready]() { ++ready; });
	EXPECT_EQ(ready, 3);
}

TEST(ColdStart, AFailingLaunchLinkIsReportedThroughOnErrorAndInitializeCompletes) // [ref]
{
	Bridge bridge;
	bridge.api.onResolve = [](const HttpRequest&) { return Respond(404); };
	EXPECT(bridge.Initialize(kInvite));
	EXPECT_EQ(bridge.errors.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(bridge.errors[0].kind, ErrorKind::LinkNotFound);
	EXPECT_EQ(bridge.errors[0].url, kInvite);
	EXPECT(!bridge.GetInitialLink().hasLink);
}

TEST(ColdStart, StartIsReadyOnlyAfterTheLaunchLinkWasResolved)
{
	Bridge bridge;
	std::uint64_t firstRequest = 0;
	bridge.api.onResolve = [](const HttpRequest&) { return Hang(); };
	bool ready = false;
	bridge.sdk->Start(&kInvite, [&ready]() { ready = true; });

	Bridge::InitialOutcome initial;
	bridge.sdk->GetInitialLink([&initial](const ResolvedLink* link) {
		initial.called = true;
		initial.hasLink = link != nullptr;
	});
	ResolvedLink unused;
	EXPECT(!bridge.sdk->TakeInitialLink(unused));
	bridge.loop.Advance(10);
	EXPECT(!ready);
	EXPECT(!bridge.sdk->IsReady());
	EXPECT(!initial.called);

	firstRequest = bridge.api.resolveIds[0];
	EXPECT(bridge.api.Complete(firstRequest, Ok()));
	EXPECT(ready);
	EXPECT(bridge.sdk->IsReady());
	EXPECT(initial.called && initial.hasLink);
	EXPECT_EQ(bridge.sdk->PendingCount(), static_cast<std::size_t>(0));
}

TEST(ColdStart, TakeInitialLinkIsTheSynchronousVariant)
{
	Bridge bridge;
	EXPECT(bridge.Initialize(kInvite));
	ResolvedLink link;
	EXPECT(bridge.sdk->TakeInitialLink(link));
	EXPECT_EQ(link.routePath, "/invite/abc123");
	EXPECT(!bridge.sdk->TakeInitialLink(link));
	bridge.Listen();
	EXPECT(bridge.received.empty());
}

TEST(ColdStart, OnlyTheLaunchLinkIsTheInitialLink)
{
	Bridge bridge;
	bridge.api.onResolve = [](const HttpRequest& request) {
		return request.url.find("invite777") != std::string::npos ? Ok("/launch") : Ok("/later");
	};
	EXPECT(bridge.Initialize(kInvite));
	bridge.sdk->HandleUrl("https://my-app.strybok.link/later");
	bridge.loop.Settle();
	EXPECT_EQ(bridge.sdk->PendingCount(), static_cast<std::size_t>(2));

	const Bridge::InitialOutcome initial = bridge.GetInitialLink();
	EXPECT(initial.hasLink);
	EXPECT_EQ(initial.link.routePath, "/launch");

	bridge.Listen();
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(bridge.received[0].routePath, "/later");
}

TEST(ColdStart, UrlsBeforeStartAreIgnored)
{
	Bridge bridge;
	bridge.Listen();
	bridge.sdk->HandleUrl(kInvite);
	bridge.loop.Settle();
	EXPECT(bridge.api.resolves.empty());
	EXPECT(bridge.received.empty());
}

// ---------------------------------------------------------------------------------------------
// running app
// ---------------------------------------------------------------------------------------------

TEST(Running, DeliversEachForegroundLinkExactlyOnce) // [ref]
{
	Bridge bridge;
	bridge.api.onResolve = [](const HttpRequest& request) {
		const std::string marker = "strybok.link%2F";
		const std::string slug = request.url.substr(request.url.find(marker) + marker.size());
		return Ok("/p/" + slug, "link_" + slug);
	};
	bridge.Listen();
	EXPECT(bridge.Initialize());

	bridge.sdk->HandleUrl("https://my-app.strybok.link/first");
	bridge.sdk->HandleUrl("https://my-app.strybok.link/second");
	bridge.sdk->HandleUrl("https://evil.example.com/third");
	bridge.loop.Settle();

	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(2));
	std::set<std::string> routes;
	for (const ResolvedLink& link : bridge.received)
	{
		routes.insert(link.routePath);
	}
	EXPECT(routes.count("/p/first") == 1 && routes.count("/p/second") == 1);
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(2));
	EXPECT(bridge.errors.empty());
}

TEST(Running, DropsAnImmediateDuplicateButHonoursTheSameLinkTappedAgainLater) // [ref]
{
	Bridge bridge;
	bridge.Listen();
	EXPECT(bridge.Initialize());

	bridge.sdk->HandleUrl(kInvite);
	bridge.sdk->HandleUrl(kInvite);
	bridge.loop.Settle();
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(1));

	// App paused, user taps the same link again a minute later (resume).
	bridge.loop.Advance(60000);
	bridge.sdk->HandleUrl(kInvite);
	bridge.loop.Settle();
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(2));
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(2));
}

TEST(Running, DedupeWindowBoundaryAndDifferentQueryStrings)
{
	Bridge bridge;
	bridge.Listen();
	EXPECT(bridge.Initialize());

	bridge.sdk->HandleUrl(kInvite);
	bridge.loop.Advance(1999);
	bridge.sdk->HandleUrl(kInvite); // 1999 ms later: still the same tap
	bridge.loop.Advance(1999);
	bridge.sdk->HandleUrl(kInvite); // every sighting restarts the window
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(1));

	bridge.loop.Advance(2000);
	bridge.sdk->HandleUrl(kInvite); // exactly one full window of silence: a new tap
	bridge.loop.Settle();
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(2));

	// The comparison is on the whole URL string, and only the last URL is remembered.
	bridge.sdk->HandleUrl(kInvite + "?a=1");
	bridge.sdk->HandleUrl(kInvite);
	bridge.loop.Settle();
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(4));
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(4));
}

TEST(Running, ForeignUrlsDoNotDisturbDuplicateDetection)
{
	Bridge bridge;
	bridge.Listen();
	EXPECT(bridge.Initialize());
	bridge.sdk->HandleUrl(kInvite);
	bridge.sdk->HandleUrl("https://evil.example.com/x");
	bridge.sdk->HandleUrl(kInvite);
	bridge.loop.Settle();
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(1));
}

TEST(Running, EveryReceiverBehindTheHandlerSeesALinkOnce) // [ref: "multiple listeners each see a link once"]
{
	// The core has one link handler; fan-out to several listeners is the wrapper's job (a
	// multicast delegate in Unreal). What the core guarantees is one handler call per link.
	Bridge bridge;
	std::vector<ResolvedLink> a;
	std::vector<ResolvedLink> b;
	bridge.sdk->SetLinkHandler([&a, &b](const ResolvedLink& link) {
		a.push_back(link);
		b.push_back(link);
	});
	bridge.sdk->SetListening(true);
	EXPECT(bridge.Initialize());
	bridge.sdk->HandleUrl(kInvite);
	bridge.loop.Settle();
	EXPECT_EQ(a.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(b.size(), static_cast<std::size_t>(1));
}

TEST(Running, ErrorsGoToOnErrorAndTheStreamKeepsWorking) // [ref]
{
	Options options;
	options.maxRetries = 0;
	Bridge bridge(options);
	bool fail = true;
	bridge.api.onResolve = [&fail](const HttpRequest&) { return fail ? NetworkError() : Ok(); };
	bridge.Listen();
	EXPECT(bridge.Initialize());

	bridge.sdk->HandleUrl("https://my-app.strybok.link/offline1");
	bridge.loop.Settle();
	EXPECT_EQ(bridge.errors.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(bridge.errors[0].kind, ErrorKind::Network);
	EXPECT_EQ(bridge.errors[0].url, "https://my-app.strybok.link/offline1");
	EXPECT(bridge.received.empty());

	fail = false;
	bridge.sdk->HandleUrl("https://my-app.strybok.link/online1");
	bridge.loop.Settle();
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(1));
}

TEST(Running, LinksQueueAgainWhileNobodyListensAndFlushInOrder)
{
	Bridge bridge;
	bridge.api.onResolve = [](const HttpRequest& request) {
		const std::string marker = "strybok.link%2F";
		return Ok("/p/" + request.url.substr(request.url.find(marker) + marker.size()));
	};
	bridge.Listen();
	EXPECT(bridge.Initialize());
	bridge.sdk->SetListening(false);

	bridge.sdk->HandleUrl("https://my-app.strybok.link/one");
	bridge.loop.Settle();
	bridge.sdk->HandleUrl("https://my-app.strybok.link/two");
	bridge.loop.Settle();
	EXPECT(bridge.received.empty());
	EXPECT_EQ(bridge.sdk->PendingCount(), static_cast<std::size_t>(2));

	bridge.sdk->SetListening(true);
	bridge.sdk->SetListening(true);
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(2));
	EXPECT_EQ(bridge.received[0].routePath, "/p/one");
	EXPECT_EQ(bridge.received[1].routePath, "/p/two");
	EXPECT_EQ(bridge.sdk->PendingCount(), static_cast<std::size_t>(0));
}

TEST(Running, ListeningWithoutAHandlerKeepsLinksQueued)
{
	Bridge bridge;
	bridge.sdk->SetListening(true);
	EXPECT(bridge.Initialize(kInvite));
	EXPECT_EQ(bridge.sdk->PendingCount(), static_cast<std::size_t>(1));
	bridge.sdk->SetLinkHandler([&bridge](const ResolvedLink& link) { bridge.received.push_back(link); });
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(1));
}

TEST(Running, AListeningProbeIsAskedAtEveryDelivery)
{
	// The Unreal wrapper polls its delegates once per frame; the probe closes the gap between
	// "the last listener unbound" and the next poll, so no link is handed to nobody.
	Bridge bridge;
	bool bound = true;
	bridge.sdk->SetListeningProbe([&bound]() { return bound; });
	bridge.Listen();
	EXPECT(bridge.Initialize());

	bound = false; // unbound, but SetListening(false) has not been called yet
	bridge.sdk->HandleUrl("https://my-app.strybok.link/one");
	bridge.loop.Settle();
	EXPECT(bridge.received.empty());
	EXPECT_EQ(bridge.sdk->PendingCount(), static_cast<std::size_t>(1));

	bridge.sdk->SetListening(true); // a poll while still unbound flushes nothing
	EXPECT(bridge.received.empty());

	bound = true;
	bridge.sdk->SetListening(true);
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(1));
	bridge.sdk->HandleUrl("https://my-app.strybok.link/two");
	bridge.loop.Settle();
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(2));
	EXPECT_EQ(bridge.sdk->PendingCount(), static_cast<std::size_t>(0));
}

TEST(Running, TheOriginalUrlIsKeptWithItsQuery)
{
	Bridge bridge;
	bridge.Listen();
	EXPECT(bridge.Initialize());
	bridge.sdk->HandleUrl("https://MY-APP.strybok.link/invite777?token=secret#f");
	bridge.loop.Settle();
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(bridge.received[0].uri, "https://MY-APP.strybok.link/invite777?token=secret#f");
	EXPECT_EQ(bridge.api.resolves[0].url, kResolvePrefix + "https%3A%2F%2Fmy-app.strybok.link%2Finvite777");
}

// ---------------------------------------------------------------------------------------------
// app_open events
// ---------------------------------------------------------------------------------------------

TEST(Events, SendsOneMinimalBestEffortEventPerDeliveredLink) // [ref]
{
	Bridge bridge;
	bridge.Listen();
	EXPECT(bridge.Initialize());
	bridge.sdk->HandleUrl(kInvite);
	bridge.loop.Settle();

	EXPECT_EQ(bridge.api.events.size(), static_cast<std::size_t>(1));
	const HttpRequest& request = bridge.api.events[0];
	EXPECT_EQ(request.method, "POST");
	EXPECT_EQ(request.url, "https://api.strybok.com/v1/public/events");
	EXPECT_EQ(request.headers.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(request.headers[0].first, "Content-Type");
	EXPECT_EQ(request.headers[0].second, "application/json");

	json::Value event;
	EXPECT(json::Parse(request.body, event));
	EXPECT(event.type == json::Type::Object);
	std::set<std::string> keys;
	for (const auto& member : event.members)
	{
		keys.insert(member.first);
	}
	const std::set<std::string> expectedKeys = {"eventId", "linkId", "type", "platform", "sdkVersion"};
	EXPECT(keys == expectedKeys);
	EXPECT_EQ(event.members.size(), static_cast<std::size_t>(5));
	EXPECT_EQ(StringMember(event, "type"), "app_open");
	EXPECT_EQ(StringMember(event, "linkId"), "link_1");
	EXPECT_EQ(StringMember(event, "platform"), "ios");
	EXPECT_EQ(StringMember(event, "sdkVersion"), "0.2.0");
	EXPECT_MSG(IsUuidV4(StringMember(event, "eventId")), StringMember(event, "eventId"));
}

TEST(Events, ASlowOrFailingEventsEndpointNeverDelaysOrBreaksDelivery) // [ref]
{
	const std::vector<std::function<Reply()>> behaviours = {
		[]() { return Hang(); },
		[]() { return NetworkError(); },
		[]() { return Respond(500); },
	};
	for (const std::function<Reply()>& behaviour : behaviours)
	{
		Bridge bridge;
		bridge.api.onEvent = behaviour;
		bridge.Listen();
		EXPECT(bridge.Initialize());
		bridge.sdk->HandleUrl(kInvite);
		bridge.loop.Settle();
		EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(1));
		EXPECT(bridge.errors.empty());
		EXPECT_EQ(bridge.api.events.size(), static_cast<std::size_t>(1));

		// Nothing is retried and nothing is left behind once the timeout has passed.
		bridge.loop.Advance(10000);
		EXPECT_EQ(bridge.api.events.size(), static_cast<std::size_t>(1));
		EXPECT(bridge.errors.empty());
		EXPECT_EQ(bridge.loop.TaskCount(), static_cast<std::size_t>(0));
	}
}

TEST(Events, CanBeTurnedOff) // [ref]
{
	Options options;
	options.reportAppOpens = false;
	Bridge bridge(options);
	EXPECT(bridge.Initialize(kInvite));
	bridge.Listen();
	bridge.loop.Settle();
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(1));
	EXPECT(bridge.api.events.empty());
}

TEST(Events, AQueuedLinkIsReportedOnceWhenResolvedNotAgainWhenDelivered)
{
	Bridge bridge;
	EXPECT(bridge.Initialize(kInvite));
	EXPECT_EQ(bridge.api.events.size(), static_cast<std::size_t>(1));
	bridge.Listen();
	bridge.loop.Settle();
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(bridge.api.events.size(), static_cast<std::size_t>(1));
}

TEST(Events, FailedLinksAreNotReported)
{
	Bridge bridge;
	bridge.api.onResolve = [](const HttpRequest&) { return Respond(404); };
	bridge.Listen();
	EXPECT(bridge.Initialize(kInvite));
	EXPECT(bridge.api.events.empty());
}

TEST(Events, EachEventHasItsOwnRandomId)
{
	Bridge bridge;
	bridge.Listen();
	EXPECT(bridge.Initialize());
	bridge.sdk->HandleUrl("https://my-app.strybok.link/one");
	bridge.sdk->HandleUrl("https://my-app.strybok.link/two");
	bridge.loop.Settle();
	EXPECT_EQ(bridge.api.events.size(), static_cast<std::size_t>(2));
	json::Value first;
	json::Value second;
	EXPECT(json::Parse(bridge.api.events[0].body, first));
	EXPECT(json::Parse(bridge.api.events[1].body, second));
	EXPECT(IsUuidV4(StringMember(first, "eventId")));
	EXPECT(IsUuidV4(StringMember(second, "eventId")));
	EXPECT(StringMember(first, "eventId") != StringMember(second, "eventId"));
}

TEST(Events, UuidFormattingForcesVersionAndVariant)
{
	std::uint8_t zeros[16] = {};
	EXPECT_EQ(FormatUuidV4(zeros), "00000000-0000-4000-8000-000000000000");
	std::uint8_t ones[16];
	FixedRandom random;
	random.Fill(ones, sizeof(ones));
	EXPECT_EQ(FormatUuidV4(ones), "ffffffff-ffff-4fff-bfff-ffffffffffff");
	EXPECT(IsUuidV4(FormatUuidV4(ones)));
}

TEST(Events, BodyIsExactAndEscapedAndTheVersionIsCapped)
{
	EXPECT_EQ(BuildAppOpenBody("id-1", "link_1", "android", "0.2.0"),
		"{\"eventId\":\"id-1\",\"linkId\":\"link_1\",\"type\":\"app_open\",\"platform\":\"android\",\"sdkVersion\":\"0.2.0\"}");

	json::Value event;
	EXPECT(json::Parse(BuildAppOpenBody("id", "li\"nk\\\n", "unknown", std::string(40, 'v')), event));
	EXPECT_EQ(StringMember(event, "linkId"), "li\"nk\\\n");
	EXPECT_EQ(StringMember(event, "sdkVersion"), std::string(32, 'v'));
	EXPECT_EQ(event.members.size(), static_cast<std::size_t>(5));

	EXPECT(json::Parse(BuildAppOpenBody("id", "l", "ios", ""), event));
	EXPECT_EQ(StringMember(event, "sdkVersion"), std::string(kSdkVersion));
	EXPECT_EQ(std::string(kSdkVersion), "0.2.0");
}

TEST(Events, AnInjectedRandomSourceIsUsed)
{
	FakeLoop loop;
	FakeApi api(loop);
	FixedRandom random;
	Config config;
	config.apiBaseUrl = kApi;
	config.allowedHosts = {kHost};
	LinkEngine sdk(config, api, loop, loop, &random);
	sdk.Start(&kInvite);
	loop.Settle();
	EXPECT_EQ(api.events.size(), static_cast<std::size_t>(1));
	json::Value event;
	EXPECT(json::Parse(api.events[0].body, event));
	EXPECT_EQ(StringMember(event, "eventId"), "ffffffff-ffff-4fff-bfff-ffffffffffff");
	EXPECT_EQ(StringMember(event, "platform"), "unknown");
}

// ---------------------------------------------------------------------------------------------
// dispose
// ---------------------------------------------------------------------------------------------

TEST(Dispose, StopsDeliveryAndRejectsFurtherUse) // [ref]
{
	Bridge bridge;
	bridge.Listen();
	EXPECT(bridge.Initialize());

	bridge.sdk->Stop();
	bridge.sdk->Stop(); // idempotent
	EXPECT(bridge.sdk->IsStopped());
	EXPECT(!bridge.sdk->IsListening());

	bridge.sdk->HandleUrl(kInvite);
	bridge.loop.Settle();
	EXPECT(bridge.received.empty());
	EXPECT(bridge.api.resolves.empty());

	bool resolveCalled = false;
	EXPECT(!bridge.sdk->Resolve(kInvite, [&resolveCalled](const ResolvedLink*, const LinkError*) { resolveCalled = true; }));
	bool readyCalled = false;
	bridge.sdk->Start(&kInvite, [&readyCalled]() { readyCalled = true; });
	bridge.sdk->SetListening(true);
	bridge.loop.Settle();
	EXPECT(!resolveCalled);
	EXPECT(!readyCalled);
	EXPECT(!bridge.sdk->IsListening());
	EXPECT(bridge.api.resolves.empty());

	const Bridge::InitialOutcome initial = bridge.GetInitialLink();
	EXPECT(initial.called && !initial.hasLink);
}

TEST(Dispose, ALinkStillResolvingWhenDisposedIsNotDelivered) // [ref]
{
	Options options;
	options.timeoutMs = 5000;
	Bridge bridge(options);
	bridge.api.onResolve = [](const HttpRequest&) { return Hang(); };
	bridge.Listen();
	EXPECT(bridge.Initialize());
	bridge.sdk->HandleUrl(kInvite);
	bridge.loop.Settle();
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(1));

	bridge.sdk->Stop();
	EXPECT_EQ(bridge.api.cancelled.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(bridge.loop.TaskCount(), static_cast<std::size_t>(0));
	EXPECT(bridge.api.Complete(bridge.api.resolveIds[0], Ok()));
	bridge.loop.Advance(10000);
	EXPECT(bridge.received.empty());
	EXPECT(bridge.errors.empty());
	EXPECT(bridge.api.events.empty());
}

TEST(Dispose, QueuedLinksAndTheInitialLinkAreDropped)
{
	Bridge bridge;
	EXPECT(bridge.Initialize(kInvite));
	EXPECT_EQ(bridge.sdk->PendingCount(), static_cast<std::size_t>(1));
	bridge.sdk->Stop();
	EXPECT_EQ(bridge.sdk->PendingCount(), static_cast<std::size_t>(0));
	ResolvedLink link;
	EXPECT(!bridge.sdk->TakeInitialLink(link));
	bridge.Listen();
	EXPECT(bridge.received.empty());
}

TEST(Dispose, DestroyingTheEngineCancelsEverythingAndLateCallbacksAreHarmless)
{
	FakeLoop loop;
	FakeApi api(loop);
	api.onResolve = [](const HttpRequest&) { return Hang(); };
	api.onEvent = []() { return Hang(); };
	int delivered = 0;
	{
		Config config;
		config.apiBaseUrl = kApi;
		config.allowedHosts = {kHost};
		config.maxRetries = 2;
		LinkEngine sdk(config, api, loop, loop);
		sdk.SetLinkHandler([&delivered](const ResolvedLink&) { ++delivered; });
		sdk.SetListening(true);
		sdk.Start(nullptr);
		sdk.HandleUrl(kInvite);
		sdk.Resolve("https://my-app.strybok.link/explicit", [&delivered](const ResolvedLink*, const LinkError*) { ++delivered; });
		loop.Settle();
		EXPECT_EQ(api.resolves.size(), static_cast<std::size_t>(2));
	}
	EXPECT_EQ(api.cancelled.size(), static_cast<std::size_t>(2));
	EXPECT_EQ(loop.TaskCount(), static_cast<std::size_t>(0));
	// A transport that still calls back after the engine is gone must not crash anything.
	EXPECT(api.Complete(api.resolveIds[0], Ok()));
	EXPECT(api.Complete(api.resolveIds[1], Ok()));
	loop.Advance(60000);
	EXPECT_EQ(delivered, 0);
}

TEST(Dispose, StoppingFromInsideALinkHandlerIsSafe)
{
	Bridge bridge;
	EXPECT(bridge.Initialize(kInvite));
	bridge.sdk->HandleUrl("https://my-app.strybok.link/second");
	bridge.loop.Settle();
	EXPECT_EQ(bridge.sdk->PendingCount(), static_cast<std::size_t>(2));

	int calls = 0;
	LinkEngine* sdk = bridge.sdk.get();
	bridge.sdk->SetLinkHandler([&calls, sdk](const ResolvedLink&) {
		++calls;
		sdk->Stop();
	});
	bridge.sdk->SetListening(true);
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(bridge.sdk->PendingCount(), static_cast<std::size_t>(0));
}

TEST(Dispose, StoppingFromInsideADirectDeliverySkipsTheEvent)
{
	Bridge bridge;
	int calls = 0;
	LinkEngine* sdk = bridge.sdk.get();
	bridge.sdk->SetLinkHandler([&calls, sdk](const ResolvedLink&) {
		++calls;
		sdk->Stop();
	});
	bridge.sdk->SetListening(true);
	bool ready = false;
	bridge.sdk->Start(&kInvite, [&ready]() { ready = true; });
	bridge.loop.Advance(10000);
	EXPECT_EQ(calls, 1);
	EXPECT(!ready);
	EXPECT(bridge.api.events.empty());
	EXPECT_EQ(bridge.loop.TaskCount(), static_cast<std::size_t>(0));
}

TEST(Dispose, DestroyingTheEngineFromInsideAnErrorHandlerIsSafe)
{
	Bridge bridge;
	bridge.api.onResolve = [](const HttpRequest&) { return Respond(404); };
	int calls = 0;
	bridge.sdk->SetErrorHandler([&calls, &bridge](const LinkError&) {
		++calls;
		bridge.sdk.reset();
	});
	bridge.sdk->Start(&kInvite);
	bridge.loop.Advance(10000);
	EXPECT_EQ(calls, 1);
	EXPECT(bridge.sdk == nullptr);
}

// ---------------------------------------------------------------------------------------------
// Creating links ("Creating links" in the SDK contract)
// ---------------------------------------------------------------------------------------------

namespace
{

const std::string kClientKey = "pk_0123456789abcdefghijABCDEFGHIJ01";
const std::string kSecretMarker = "SECRET-PAYLOAD-MARKER";

struct ShareOptions
{
	int maxRetries = 2;
	std::int64_t timeoutMs = 60;
	std::int64_t retryDelayMs = 1;
	std::string apiBaseUrl = kApi;
	std::string clientKey = kClientKey;
};

/** A `LinkCreator` wired to fakes. No `LinkEngine` exists, so nothing was initialized or started. */
struct Share
{
	FakeLoop loop;
	FakeApi api{loop};
	std::unique_ptr<LinkCreator> creator;

	explicit Share(const ShareOptions& options = ShareOptions())
	{
		Config config;
		config.apiBaseUrl = options.apiBaseUrl;
		config.clientKey = options.clientKey;
		config.timeoutMs = options.timeoutMs;
		config.maxRetries = options.maxRetries;
		config.retryDelayMs = options.retryDelayMs;
		// allowedHosts stays empty on purpose: creating links does not need it.
		creator.reset(new LinkCreator(config, api, loop));
	}

	struct Outcome
	{
		int calls = 0;
		bool ok = false;
		/** Whether the callback ran before `Create` returned. */
		bool synchronous = false;
		bool accepted = false;
		CreatedLink link;
		LinkError error;
	};

	Outcome Create(const CreateLinkParams& params, std::int64_t waitMs = 1000)
	{
		Outcome outcome;
		outcome.accepted = creator->Create(params, [&outcome](const CreatedLink* link, const LinkError* error) {
			++outcome.calls;
			outcome.ok = link != nullptr && error == nullptr;
			if (link != nullptr)
			{
				outcome.link = *link;
			}
			if (error != nullptr)
			{
				outcome.error = *error;
			}
		});
		outcome.synchronous = outcome.calls == 1;
		loop.Advance(waitMs);
		return outcome;
	}
};

CreateLinkParams Product()
{
	CreateLinkParams params;
	params.routePath = "/product/42";
	return params;
}

CreateLinkParams FullProduct()
{
	CreateLinkParams params;
	params.routePath = "/product/42";
	params.payloadJson = "{\"productId\":42,\"note\":\"" + kSecretMarker + "\"}";
	params.title = "Red shoes";
	params.description = "Look at these";
	params.imageUrl = "https://cdn.example.com/shoes.png";
	return params;
}

std::string Header(const HttpRequest& request, const std::string& name)
{
	std::string found = "<missing>";
	int count = 0;
	for (const auto& header : request.headers)
	{
		std::string lowered;
		for (char c : header.first)
		{
			lowered.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
		}
		if (lowered == name)
		{
			found = header.second;
			++count;
		}
	}
	return count > 1 ? std::string("<repeated>") : found;
}

std::set<std::string> Keys(const json::Value& object)
{
	std::set<std::string> keys;
	for (const auto& member : object.members)
	{
		keys.insert(member.first);
	}
	return keys;
}

bool Mentions(const LinkError& error, const std::string& needle)
{
	if (error.message.find(needle) != std::string::npos || error.url.find(needle) != std::string::npos ||
		error.routePath.find(needle) != std::string::npos)
	{
		return true;
	}
	for (const ValidationIssue& issue : error.issues)
	{
		if (issue.path.find(needle) != std::string::npos || issue.message.find(needle) != std::string::npos)
		{
			return true;
		}
	}
	return false;
}

} // namespace

TEST(CreateLink, Status201ReturnsTheCreatedLinkAndSendsTheContractRequest)
{
	Share share;
	const Share::Outcome outcome = share.Create(FullProduct());

	EXPECT(outcome.accepted);
	EXPECT_EQ(outcome.calls, 1);
	EXPECT(outcome.ok);
	EXPECT(!outcome.synchronous);
	EXPECT_EQ(outcome.link.url, "https://my-app.strybok.link/Ab3xK9");
	EXPECT_EQ(outcome.link.slug, "Ab3xK9");
	EXPECT_EQ(outcome.link.routePath, "/product/42");
	EXPECT_EQ(outcome.link.linkId, "link_9");

	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(1));
	const HttpRequest& request = share.api.creates[0];
	EXPECT_EQ(request.method, "POST");
	EXPECT_EQ(request.url, "https://api.strybok.com/v1/public/links");
	EXPECT_EQ(request.headers.size(), static_cast<std::size_t>(3));
	EXPECT_EQ(Header(request, "content-type"), "application/json");
	EXPECT_EQ(Header(request, "accept"), "application/json");
	EXPECT_EQ(Header(request, "x-strybok-key"), kClientKey);
	EXPECT_EQ(Header(request, "authorization"), "<missing>");
	EXPECT(request.url.find(kClientKey) == std::string::npos);
	EXPECT(request.body.find(kClientKey) == std::string::npos);

	EXPECT_EQ(request.body,
		"{\"routePath\":\"/product/42\",\"payload\":{\"productId\":42,\"note\":\"SECRET-PAYLOAD-MARKER\"},"
		"\"title\":\"Red shoes\",\"description\":\"Look at these\",\"imageUrl\":\"https://cdn.example.com/shoes.png\"}");
	json::Value body;
	EXPECT(json::Parse(request.body, body));
	const std::set<std::string> expectedKeys = {"routePath", "payload", "title", "description", "imageUrl"};
	EXPECT(Keys(body) == expectedKeys);

	EXPECT_EQ(share.creator->InFlightCount(), static_cast<std::size_t>(0));
	EXPECT_EQ(share.loop.TaskCount(), static_cast<std::size_t>(0));
	EXPECT(share.api.resolves.empty());
	EXPECT(share.api.events.empty());
}

TEST(CreateLink, Status200MeansAnIdenticalLinkExistedAndIsSuccessToo)
{
	Share share;
	share.api.onCreate = [](const HttpRequest&) { return Respond(200, CreatedBody()); };
	const Share::Outcome outcome = share.Create(Product());
	EXPECT(outcome.ok);
	EXPECT_EQ(outcome.link.url, "https://my-app.strybok.link/Ab3xK9");
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(1));
}

TEST(CreateLink, ArgumentsThatWereNotGivenAreOmittedFromTheBody)
{
	Share share;
	EXPECT(share.Create(Product()).ok);
	EXPECT_EQ(share.api.creates[0].body, "{\"routePath\":\"/product/42\"}");
	EXPECT(share.api.creates[0].body.find("null") == std::string::npos);

	CreateLinkParams params = Product();
	params.title = "Only a title";
	EXPECT(share.Create(params).ok);
	EXPECT_EQ(share.api.creates[1].body, "{\"routePath\":\"/product/42\",\"title\":\"Only a title\"}");

	params = Product();
	params.payloadJson = "  {\"a\":[1,{\"b\":null}]}\n";
	EXPECT(share.Create(params).ok);
	EXPECT_EQ(share.api.creates[2].body, "{\"routePath\":\"/product/42\",\"payload\":{\"a\":[1,{\"b\":null}]}}");

	params = Product();
	params.payloadJson = "{}";
	params.imageUrl = "https://cdn.example.com/a.png";
	EXPECT(share.Create(params).ok);
	EXPECT_EQ(share.api.creates[3].body,
		"{\"routePath\":\"/product/42\",\"payload\":{},\"imageUrl\":\"https://cdn.example.com/a.png\"}");
}

TEST(CreateLink, TextIsEscapedInTheBody)
{
	Share share;
	CreateLinkParams params;
	params.routePath = "/say/\"hi\"\\there";
	params.title = "line1\nline2\ttab";
	params.description = std::string("ctl\x01", 4);
	EXPECT(share.Create(params).ok);
	json::Value body;
	EXPECT(json::Parse(share.api.creates[0].body, body));
	EXPECT_EQ(StringMember(body, "routePath"), "/say/\"hi\"\\there");
	EXPECT_EQ(StringMember(body, "title"), "line1\nline2\ttab");
	EXPECT_EQ(StringMember(body, "description"), std::string("ctl\x01", 4));
}

TEST(CreateLink, KeepsAPathPrefixOfTheApiBaseUrl)
{
	ShareOptions options;
	options.apiBaseUrl = "https://example.com/strybok/";
	Share share(options);
	EXPECT(share.Create(Product()).ok);
	EXPECT_EQ(share.api.creates[0].url, "https://example.com/strybok/v1/public/links");
	EXPECT_EQ(BuildLinksEndpoint("https://api.strybok.com"), "https://api.strybok.com/v1/public/links");
}

TEST(CreateLink, WithoutAClientKeyItFailsAsUnauthorizedWithoutANetworkCall)
{
	const std::vector<std::string> keys = {"", "   ", "\t\n", "pk_with space", "pk_line\nbreak", "pk_\xC3\xA9"};
	for (const std::string& key : keys)
	{
		ShareOptions options;
		options.clientKey = key;
		Share share(options);
		const Share::Outcome outcome = share.Create(FullProduct());
		EXPECT(outcome.accepted);
		EXPECT_EQ(outcome.calls, 1);
		EXPECT(outcome.synchronous);
		EXPECT(!outcome.ok);
		EXPECT_EQ(outcome.error.kind, ErrorKind::Unauthorized);
		EXPECT_EQ(outcome.error.statusCode, 0);
		EXPECT(share.api.creates.empty());
		EXPECT_EQ(share.loop.TaskCount(), static_cast<std::size_t>(0));
	}
	EXPECT_EQ(NormalizeClientKey("  pk_abc \n"), "pk_abc");
}

TEST(CreateLink, TheMissingKeyIsReportedBeforeABadRoute)
{
	ShareOptions options;
	options.clientKey = "";
	Share share(options);
	CreateLinkParams params;
	params.routePath = "no-slash";
	EXPECT_EQ(share.Create(params).error.kind, ErrorKind::Unauthorized);
}

TEST(CreateLink, ABadRoutePathFailsAsValidationWithoutANetworkCall)
{
	const std::vector<std::string> routes = {
		"", "product/42", "//evil.example.com/x", "//", "https://evil.example.com/x", " /product",
		"/" + std::string(512, 'a'),
	};
	for (const std::string& route : routes)
	{
		Share share;
		CreateLinkParams params = FullProduct();
		params.routePath = route;
		const Share::Outcome outcome = share.Create(params);
		EXPECT_MSG(outcome.accepted && outcome.calls == 1 && outcome.synchronous && !outcome.ok, route);
		EXPECT_EQ(outcome.error.kind, ErrorKind::Validation);
		EXPECT_EQ(outcome.error.statusCode, 0);
		EXPECT(!outcome.error.message.empty());
		EXPECT(outcome.error.issues.empty());
		EXPECT(share.api.creates.empty());
		EXPECT_EQ(share.loop.TaskCount(), static_cast<std::size_t>(0));
	}
}

TEST(CreateLink, RoutePathLengthIsCountedInCharactersUpTo512)
{
	Share share;
	CreateLinkParams params;
	params.routePath = "/" + std::string(511, 'a');
	EXPECT(share.Create(params).ok);

	params.routePath = "/";
	EXPECT(share.Create(params).ok);

	// 511 two-byte characters are 1023 bytes but 512 characters with the slash.
	std::string accented = "/";
	for (int index = 0; index < 511; ++index)
	{
		accented += "\xC3\xA9";
	}
	params.routePath = accented;
	EXPECT(share.Create(params).ok);
	params.routePath = accented + "\xC3\xA9";
	EXPECT_EQ(share.Create(params).error.kind, ErrorKind::Validation);

	// A character outside the basic plane counts as two, like on the server.
	std::string astral = "/" + std::string(509, 'a') + "\xF0\x9F\x98\x80";
	params.routePath = astral;
	EXPECT(share.Create(params).ok);
	params.routePath = astral + "a";
	EXPECT_EQ(share.Create(params).error.kind, ErrorKind::Validation);
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(4));
}

TEST(CreateLink, APayloadThatIsNotAJsonObjectFailsAsValidationWithoutANetworkCall)
{
	const std::vector<std::string> payloads = {
		"[1,2]", "42", "\"text\"", "true", "null", "{\"a\":", "{'a':1}", "{\"a\":1} trailing", " ",
		"{\"note\":\"" + kSecretMarker + "\"",
	};
	for (const std::string& payload : payloads)
	{
		Share share;
		CreateLinkParams params = Product();
		params.payloadJson = payload;
		const Share::Outcome outcome = share.Create(params);
		EXPECT_MSG(outcome.accepted && outcome.calls == 1 && outcome.synchronous && !outcome.ok, payload);
		EXPECT_EQ(outcome.error.kind, ErrorKind::Validation);
		EXPECT_EQ(outcome.error.message, "payload must be a JSON object");
		EXPECT(!Mentions(outcome.error, kSecretMarker));
		EXPECT(share.api.creates.empty());
	}
}

TEST(CreateLink, AMalformedSuccessBodyIsAServerErrorWithTheStatusAndIsNotRetried)
{
	const std::vector<std::string> bodies = {
		"",
		"not json",
		"[]",
		"\"https://my-app.strybok.link/Ab3xK9\"",
		"{}",
		"{\"url\":\"https://my-app.strybok.link/Ab3xK9\",\"slug\":\"Ab3xK9\",\"routePath\":\"/product/42\"}",
		"{\"linkId\":\"link_9\",\"slug\":\"Ab3xK9\",\"routePath\":\"/product/42\"}",
		"{\"linkId\":\"link_9\",\"url\":\"https://my-app.strybok.link/Ab3xK9\",\"routePath\":\"/product/42\"}",
		"{\"linkId\":\"link_9\",\"url\":\"https://my-app.strybok.link/Ab3xK9\",\"slug\":\"Ab3xK9\"}",
		"{\"linkId\":9,\"url\":\"https://my-app.strybok.link/Ab3xK9\",\"slug\":\"Ab3xK9\",\"routePath\":\"/product/42\"}",
		"{\"linkId\":\"link_9\",\"url\":\"https://my-app.strybok.link/Ab3xK9\",\"slug\":null,\"routePath\":\"/product/42\"}",
		"{\"linkId\":\"link_9\",\"url\":\"http://my-app.strybok.link/Ab3xK9\",\"slug\":\"Ab3xK9\",\"routePath\":\"/product/42\"}",
		"{\"linkId\":\"link_9\",\"url\":\"myapp://Ab3xK9\",\"slug\":\"Ab3xK9\",\"routePath\":\"/product/42\"}",
		"{\"linkId\":\"link_9\",\"url\":\"\",\"slug\":\"Ab3xK9\",\"routePath\":\"/product/42\"}",
		"{\"linkId\":\"link_9\",\"url\":[\"https://x\"],\"slug\":\"Ab3xK9\",\"routePath\":\"/product/42\"}",
	};
	for (const int status : {200, 201})
	{
		for (const std::string& body : bodies)
		{
			Share share;
			share.api.onCreate = [status, body](const HttpRequest&) { return Respond(status, body); };
			const Share::Outcome outcome = share.Create(Product());
			EXPECT_MSG(outcome.calls == 1 && !outcome.ok, body);
			EXPECT_EQ(outcome.error.kind, ErrorKind::Server);
			EXPECT_EQ(outcome.error.statusCode, status);
			EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(1));
		}
	}
}

TEST(CreateLink, TheCreatedUrlIsNotCheckedAgainstAllowedHosts)
{
	Share share;
	share.api.onCreate = [](const HttpRequest&) {
		return Respond(201, "{\"linkId\":\"l\",\"url\":\"https://links.custom-domain.example/Zz9\",\"slug\":\"Zz9\","
			"\"routePath\":\"/product/42\",\"extra\":true}");
	};
	const Share::Outcome outcome = share.Create(Product());
	EXPECT(outcome.ok);
	EXPECT_EQ(outcome.link.url, "https://links.custom-domain.example/Zz9");
}

TEST(CreateLink, Status401IsUnauthorizedAndIsNotRetried)
{
	Share share;
	share.api.onCreate = [](const HttpRequest&) {
		return Respond(401, "{\"error\":{\"code\":\"UNAUTHENTICATED\",\"message\":\"Authentication required\",\"details\":{}}}");
	};
	const Share::Outcome outcome = share.Create(FullProduct());
	EXPECT_EQ(outcome.calls, 1);
	EXPECT(!outcome.ok);
	EXPECT_EQ(outcome.error.kind, ErrorKind::Unauthorized);
	EXPECT_EQ(outcome.error.statusCode, 401);
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(1));
}

TEST(CreateLink, Status403IsRouteNotAllowedWithTheRouteAndIsNotRetried)
{
	Share share;
	share.api.onCreate = [](const HttpRequest&) {
		return Respond(403, "{\"error\":{\"code\":\"ROUTE_NOT_ALLOWED\",\"message\":\"Route not allowed\",\"details\":{}}}");
	};
	const Share::Outcome outcome = share.Create(FullProduct());
	EXPECT_EQ(outcome.calls, 1);
	EXPECT_EQ(outcome.error.kind, ErrorKind::RouteNotAllowed);
	EXPECT_EQ(outcome.error.routePath, "/product/42");
	EXPECT_EQ(outcome.error.statusCode, 403);
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(1));
}

TEST(CreateLink, Status429IsRateLimitedWithRetryAfterAndIsNeverRetried)
{
	Share share;
	share.api.onCreate = [](const HttpRequest&) {
		Reply reply = Respond(429, "{\"error\":{\"code\":\"RATE_LIMITED\",\"message\":\"Too many\",\"details\":{\"period\":\"day\"}}}");
		reply.retryAfter = "60";
		return reply;
	};
	Share::Outcome outcome = share.Create(FullProduct());
	EXPECT_EQ(outcome.calls, 1);
	EXPECT_EQ(outcome.error.kind, ErrorKind::RateLimited);
	EXPECT_EQ(outcome.error.retryAfterSeconds, static_cast<std::int64_t>(60));
	EXPECT_EQ(outcome.error.statusCode, 429);
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(1));

	share.api.onCreate = [](const HttpRequest&) { return Respond(429); };
	outcome = share.Create(Product());
	EXPECT_EQ(outcome.error.kind, ErrorKind::RateLimited);
	EXPECT_EQ(outcome.error.retryAfterSeconds, static_cast<std::int64_t>(-1));
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(2));
}

TEST(CreateLink, RetryAfterIsReadAsWholeSeconds)
{
	EXPECT_EQ(ParseRetryAfterSeconds("60"), static_cast<std::int64_t>(60));
	EXPECT_EQ(ParseRetryAfterSeconds(" 86400 "), static_cast<std::int64_t>(86400));
	EXPECT_EQ(ParseRetryAfterSeconds("0"), static_cast<std::int64_t>(0));
	EXPECT_EQ(ParseRetryAfterSeconds(""), static_cast<std::int64_t>(-1));
	EXPECT_EQ(ParseRetryAfterSeconds("   "), static_cast<std::int64_t>(-1));
	EXPECT_EQ(ParseRetryAfterSeconds("-5"), static_cast<std::int64_t>(-1));
	EXPECT_EQ(ParseRetryAfterSeconds("1.5"), static_cast<std::int64_t>(-1));
	EXPECT_EQ(ParseRetryAfterSeconds("soon"), static_cast<std::int64_t>(-1));
	EXPECT_EQ(ParseRetryAfterSeconds("Wed, 21 Oct 2026 07:28:00 GMT"), static_cast<std::int64_t>(-1));
	EXPECT_EQ(ParseRetryAfterSeconds("99999999999999999999999999"), static_cast<std::int64_t>(2147483647));
}

TEST(CreateLink, Status400And413And422AreValidationWithTheServersMessageAndIssues)
{
	for (const int status : {400, 413, 422})
	{
		Share share;
		share.api.onCreate = [status](const HttpRequest&) {
			return Respond(status,
				"{\"error\":{\"code\":\"VALIDATION_ERROR\",\"message\":\"The request is invalid\",\"details\":{\"issues\":["
				"{\"path\":\"title\",\"message\":\"Too long\"},{\"path\":\"\",\"message\":\"Unrecognized key\"},"
				"\"junk\",{\"path\":7}]}}}");
		};
		const Share::Outcome outcome = share.Create(FullProduct());
		EXPECT_EQ(outcome.calls, 1);
		EXPECT(!outcome.ok);
		EXPECT_EQ(outcome.error.kind, ErrorKind::Validation);
		EXPECT_EQ(outcome.error.statusCode, status);
		EXPECT_EQ(outcome.error.message, "The request is invalid");
		EXPECT_EQ(outcome.error.issues.size(), static_cast<std::size_t>(2));
		if (outcome.error.issues.size() == 2)
		{
			EXPECT_EQ(outcome.error.issues[0].path, "title");
			EXPECT_EQ(outcome.error.issues[0].message, "Too long");
			EXPECT_EQ(outcome.error.issues[1].path, "");
			EXPECT_EQ(outcome.error.issues[1].message, "Unrecognized key");
		}
		EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(1));
	}
}

TEST(CreateLink, AValidationAnswerWithoutAReadableBodyStillIsValidation)
{
	const std::vector<std::string> bodies = {"", "<html>", "[]", "{\"error\":\"nope\"}", "{\"error\":{\"message\":7,\"details\":[]}}"};
	for (const std::string& body : bodies)
	{
		Share share;
		share.api.onCreate = [body](const HttpRequest&) { return Respond(413, body); };
		const Share::Outcome outcome = share.Create(Product());
		EXPECT_EQ(outcome.error.kind, ErrorKind::Validation);
		EXPECT_EQ(outcome.error.statusCode, 413);
		EXPECT(!outcome.error.message.empty());
		EXPECT(outcome.error.issues.empty());
	}
}

TEST(CreateLink, OtherClientErrorsAndUnexpectedStatusesAreServerErrorsAndAreNotRetried)
{
	for (const int status : {402, 404, 405, 409, 418, 202, 204, 301, 304})
	{
		Share share;
		share.api.onCreate = [status](const HttpRequest&) { return Respond(status, CreatedBody()); };
		const Share::Outcome outcome = share.Create(Product());
		EXPECT_EQ(outcome.calls, 1);
		EXPECT(!outcome.ok);
		EXPECT_EQ(outcome.error.kind, ErrorKind::Server);
		EXPECT_EQ(outcome.error.statusCode, status);
		EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(1));
	}
}

TEST(CreateLink, ServerErrorsAreRetriedWithADoublingPauseAndTheSameBody)
{
	ShareOptions options;
	options.retryDelayMs = 400;
	options.timeoutMs = 5000;
	Share share(options);
	int calls = 0;
	share.api.onCreate = [&calls](const HttpRequest&) {
		++calls;
		return calls < 3 ? Respond(503, "unavailable") : Respond(201, CreatedBody());
	};
	const Share::Outcome outcome = share.Create(FullProduct(), 10000);
	EXPECT(outcome.ok);
	EXPECT_EQ(outcome.calls, 1);
	EXPECT_EQ(calls, 3);
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(3));

	std::vector<std::int64_t> pauses;
	for (const std::int64_t delay : share.loop.scheduledDelays)
	{
		if (delay != 5000 && delay != 0)
		{
			pauses.push_back(delay);
		}
	}
	EXPECT_EQ(pauses.size(), static_cast<std::size_t>(2));
	if (pauses.size() == 2)
	{
		EXPECT_EQ(pauses[0], static_cast<std::int64_t>(400));
		EXPECT_EQ(pauses[1], static_cast<std::int64_t>(800));
	}

	for (const HttpRequest& request : share.api.creates)
	{
		EXPECT_EQ(request.body, share.api.creates[0].body);
		EXPECT_EQ(request.url, share.api.creates[0].url);
		EXPECT_EQ(request.method, "POST");
		EXPECT(request.headers == share.api.creates[0].headers);
	}
	EXPECT_EQ(share.loop.TaskCount(), static_cast<std::size_t>(0));
}

TEST(CreateLink, AServerErrorAfterAllRetriesReportsTheLastStatus)
{
	Share share;
	int calls = 0;
	share.api.onCreate = [&calls](const HttpRequest&) {
		++calls;
		return Respond(calls < 3 ? 500 : 502);
	};
	const Share::Outcome outcome = share.Create(Product());
	EXPECT_EQ(outcome.calls, 1);
	EXPECT_EQ(outcome.error.kind, ErrorKind::Server);
	EXPECT_EQ(outcome.error.statusCode, 502);
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(3));
}

TEST(CreateLink, TransportFailuresAreRetriedAndThenReportedAsNetwork)
{
	Share share;
	share.api.onCreate = [](const HttpRequest&) { return NetworkError(); };
	Share::Outcome outcome = share.Create(FullProduct());
	EXPECT_EQ(outcome.calls, 1);
	EXPECT(!outcome.ok);
	EXPECT_EQ(outcome.error.kind, ErrorKind::Network);
	EXPECT_EQ(outcome.error.statusCode, 0);
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(3));
	EXPECT_EQ(share.api.creates[2].body, share.api.creates[0].body);

	// A retry after a lost response gets the existing link back (200).
	int calls = 0;
	share.api.onCreate = [&calls](const HttpRequest&) {
		++calls;
		return calls == 1 ? NetworkError() : Respond(200, CreatedBody());
	};
	outcome = share.Create(Product());
	EXPECT(outcome.ok);
	EXPECT_EQ(calls, 2);
}

TEST(CreateLink, NoAnswerInTimeIsRetriedAndThenReportedAsTimeout)
{
	Share share;
	share.api.onCreate = [](const HttpRequest&) { return Hang(); };
	const Share::Outcome outcome = share.Create(FullProduct());
	EXPECT_EQ(outcome.calls, 1);
	EXPECT(!outcome.ok);
	EXPECT_EQ(outcome.error.kind, ErrorKind::Timeout);
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(3));
	EXPECT_EQ(share.api.creates[1].body, share.api.creates[0].body);
	EXPECT_EQ(share.api.creates[2].body, share.api.creates[0].body);
	// Every attempt that was given up on is cancelled.
	EXPECT(share.api.cancelled == share.api.createIds);

	// A late answer to an abandoned attempt changes nothing.
	EXPECT(share.api.Complete(share.api.createIds[0], Respond(201, CreatedBody())));
	EXPECT_EQ(outcome.calls, 1);
	EXPECT_EQ(share.loop.TaskCount(), static_cast<std::size_t>(0));
}

TEST(CreateLink, TheTimeoutAppliesToEachAttempt)
{
	ShareOptions options;
	options.maxRetries = 1;
	options.timeoutMs = 100;
	options.retryDelayMs = 10;
	Share share(options);
	share.api.onCreate = [](const HttpRequest&) { return Hang(); };
	int calls = 0;
	LinkError last;
	share.creator->Create(Product(), [&calls, &last](const CreatedLink*, const LinkError* error) {
		++calls;
		if (error != nullptr)
		{
			last = *error;
		}
	});
	share.loop.Advance(99);
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(1));
	share.loop.Advance(1 + 10);
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(2));
	EXPECT_EQ(calls, 0);
	share.loop.Advance(99);
	EXPECT_EQ(calls, 0);
	share.loop.Advance(1);
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(last.kind, ErrorKind::Timeout);
}

TEST(CreateLink, ZeroRetriesMeansASingleAttempt)
{
	ShareOptions options;
	options.maxRetries = 0;
	Share share(options);
	share.api.onCreate = [](const HttpRequest&) { return Respond(503); };
	const Share::Outcome outcome = share.Create(Product());
	EXPECT_EQ(outcome.error.kind, ErrorKind::Server);
	EXPECT_EQ(outcome.error.statusCode, 503);
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(1));
}

TEST(CreateLink, ErrorsNeverContainTheClientKeyOrThePayload)
{
	struct Case
	{
		std::function<Reply(const HttpRequest&)> reply;
		ErrorKind kind;
	};
	const std::vector<Case> cases = {
		{[](const HttpRequest&) { return Respond(401); }, ErrorKind::Unauthorized},
		{[](const HttpRequest&) { return Respond(403); }, ErrorKind::RouteNotAllowed},
		{[](const HttpRequest&) { return Respond(429); }, ErrorKind::RateLimited},
		{[](const HttpRequest&) { return Respond(422, "{\"error\":{\"message\":\"The request is invalid\"}}"); }, ErrorKind::Validation},
		{[](const HttpRequest&) { return Respond(404); }, ErrorKind::Server},
		{[](const HttpRequest&) { return Respond(500); }, ErrorKind::Server},
		{[](const HttpRequest&) { return Respond(201, "{}"); }, ErrorKind::Server},
		{[](const HttpRequest&) { return NetworkError(); }, ErrorKind::Network},
		{[](const HttpRequest&) { return Hang(); }, ErrorKind::Timeout},
	};
	for (const Case& entry : cases)
	{
		Share share;
		share.api.onCreate = entry.reply;
		const Share::Outcome outcome = share.Create(FullProduct());
		EXPECT_EQ(outcome.calls, 1);
		EXPECT_EQ(outcome.error.kind, entry.kind);
		EXPECT(!Mentions(outcome.error, kClientKey));
		EXPECT(!Mentions(outcome.error, "pk_"));
		EXPECT(!Mentions(outcome.error, kSecretMarker));
		EXPECT(!Mentions(outcome.error, "productId"));
		EXPECT(outcome.error.url.empty());
	}

	// Rejected before sending: a bad route next to a payload, and a bad payload.
	Share share;
	CreateLinkParams params = FullProduct();
	params.routePath = "nope";
	Share::Outcome outcome = share.Create(params);
	EXPECT(!Mentions(outcome.error, kClientKey));
	EXPECT(!Mentions(outcome.error, kSecretMarker));
	params = FullProduct();
	params.payloadJson = "[\"" + kSecretMarker + "\"]";
	outcome = share.Create(params);
	EXPECT_EQ(outcome.error.kind, ErrorKind::Validation);
	EXPECT(!Mentions(outcome.error, kSecretMarker));
}

TEST(CreateLink, AnAnswerInsideSendIsHandled)
{
	Share share;
	share.api.synchronous = true;
	Share::Outcome outcome = share.Create(Product());
	EXPECT(outcome.ok);
	EXPECT(outcome.synchronous);
	EXPECT_EQ(share.loop.TaskCount(), static_cast<std::size_t>(0));

	share.api.onCreate = [](const HttpRequest&) { return Respond(500); };
	outcome = share.Create(Product());
	EXPECT_EQ(outcome.calls, 1);
	EXPECT_EQ(outcome.error.kind, ErrorKind::Server);
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(4));
	EXPECT_EQ(share.loop.TaskCount(), static_cast<std::size_t>(0));
}

TEST(CreateLink, SeveralCallsRunIndependently)
{
	Share share;
	share.api.onCreate = [](const HttpRequest& request) {
		return request.body.find("/slow") != std::string::npos ? Hang() : Respond(201, CreatedBody("/fast"));
	};
	int slowCalls = 0;
	int fastCalls = 0;
	CreateLinkParams slow;
	slow.routePath = "/slow";
	CreateLinkParams fast;
	fast.routePath = "/fast";
	share.creator->Create(slow, [&slowCalls](const CreatedLink*, const LinkError*) { ++slowCalls; });
	share.creator->Create(fast, [&fastCalls](const CreatedLink* link, const LinkError*) {
		fastCalls += link != nullptr ? 1 : 100;
	});
	EXPECT_EQ(share.creator->InFlightCount(), static_cast<std::size_t>(2));
	share.loop.Settle();
	EXPECT_EQ(fastCalls, 1);
	EXPECT_EQ(slowCalls, 0);
	EXPECT_EQ(share.creator->InFlightCount(), static_cast<std::size_t>(1));
	share.loop.Advance(10000);
	EXPECT_EQ(slowCalls, 1);
	EXPECT_EQ(share.creator->InFlightCount(), static_cast<std::size_t>(0));
}

TEST(CreateLink, WorksWithoutAnEngineAndWhileOneIsNeitherStartedNorListening)
{
	// `Share` has no LinkEngine at all and no allowed hosts.
	Share share;
	Config config;
	config.apiBaseUrl = kApi;
	config.clientKey = kClientKey;
	EXPECT(ValidateCreateLinkConfig(config).empty());
	EXPECT(!ValidateConfig(config).empty()); // the receiving side does need allowedHosts
	config.apiBaseUrl = "api.strybok.com";
	EXPECT(!ValidateCreateLinkConfig(config).empty());
	EXPECT(share.Create(Product()).ok);

	// Next to an engine that was never started: both work on their own.
	Bridge bridge;
	Config shareConfig;
	shareConfig.apiBaseUrl = kApi;
	shareConfig.clientKey = kClientKey;
	LinkCreator creator(shareConfig, bridge.api, bridge.loop);
	bool created = false;
	creator.Create(Product(), [&created](const CreatedLink* link, const LinkError*) { created = link != nullptr; });
	bridge.loop.Settle();
	EXPECT(created);
	EXPECT(!bridge.sdk->IsStarted());
	EXPECT(bridge.api.resolves.empty());
	EXPECT(bridge.api.events.empty());
	EXPECT(bridge.errors.empty());
}

TEST(CreateLink, IsNeverOnTheStartPathAndItsErrorsDoNotReachOnError)
{
	Bridge bridge;
	bridge.Listen();
	EXPECT(bridge.Initialize(kInvite));
	bridge.sdk->HandleUrl("https://my-app.strybok.link/second");
	bridge.loop.Advance(10000);
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(2));
	EXPECT(bridge.api.creates.empty()); // starting, resolving and delivering never create links

	Config config;
	config.apiBaseUrl = kApi;
	config.clientKey = kClientKey;
	config.maxRetries = 0;
	LinkCreator creator(config, bridge.api, bridge.loop);
	bridge.api.onCreate = [](const HttpRequest&) { return Respond(401); };
	int failures = 0;
	creator.Create(Product(), [&failures](const CreatedLink*, const LinkError* error) {
		failures += error != nullptr ? 1 : 0;
	});
	bridge.loop.Settle();
	EXPECT_EQ(failures, 1);
	EXPECT(bridge.errors.empty());
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(2));

	// A hanging create does not hold up a link that arrives meanwhile.
	bridge.api.onCreate = [](const HttpRequest&) { return Hang(); };
	creator.Create(Product(), nullptr);
	bridge.loop.Advance(5000);
	bridge.sdk->HandleUrl("https://my-app.strybok.link/third");
	bridge.loop.Settle();
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(3));
}

TEST(CreateLink, StopDropsCallbacksCancelsRequestsAndRejectsFurtherUse)
{
	Share share;
	share.api.onCreate = [](const HttpRequest&) { return Hang(); };
	int calls = 0;
	EXPECT(share.creator->Create(Product(), [&calls](const CreatedLink*, const LinkError*) { ++calls; }));
	share.loop.Advance(10);
	share.creator->Stop();
	EXPECT(share.creator->IsStopped());
	EXPECT_EQ(share.api.cancelled.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(share.loop.TaskCount(), static_cast<std::size_t>(0));
	EXPECT(share.api.Complete(share.api.createIds[0], Respond(201, CreatedBody())));
	share.loop.Advance(10000);
	EXPECT_EQ(calls, 0);
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(1));

	EXPECT(!share.creator->Create(Product(), [&calls](const CreatedLink*, const LinkError*) { ++calls; }));
	EXPECT_EQ(calls, 0);
	EXPECT_EQ(share.api.creates.size(), static_cast<std::size_t>(1));
	share.creator->Stop(); // harmless
}

TEST(CreateLink, DestroyingTheCreatorFromInsideItsCallbackIsSafe)
{
	Share share;
	int calls = 0;
	share.creator->Create(Product(), [&calls, &share](const CreatedLink*, const LinkError*) {
		++calls;
		share.creator.reset();
	});
	share.loop.Advance(10000);
	EXPECT_EQ(calls, 1);
	EXPECT(share.creator == nullptr);

	// Also when the callback runs before `Create` returns, and while a retry pause is pending.
	Share early;
	early.api.synchronous = true;
	early.creator->Create(Product(), [&calls, &early](const CreatedLink*, const LinkError*) {
		++calls;
		early.creator.reset();
	});
	EXPECT_EQ(calls, 2);

	ShareOptions options;
	options.clientKey = "";
	Share keyless(options);
	keyless.creator->Create(Product(), [&calls, &keyless](const CreatedLink*, const LinkError*) {
		++calls;
		keyless.creator.reset();
	});
	EXPECT_EQ(calls, 3);

	Share retrying;
	retrying.api.onCreate = [](const HttpRequest&) { return Respond(500); };
	retrying.creator->Create(Product(), nullptr);
	retrying.loop.Settle();
	retrying.creator.reset();
	retrying.loop.Advance(10000);
	EXPECT_EQ(retrying.loop.TaskCount(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------------------------

int main()
{
	int failedTests = 0;
	const char* currentGroup = "";
	for (const TestCase& test : Registry())
	{
		if (std::string(currentGroup) != test.group)
		{
			currentGroup = test.group;
			std::printf("%s\n", currentGroup);
		}
		gFailuresInTest = 0;
		test.run();
		std::printf("  %s %s\n", gFailuresInTest == 0 ? "ok  " : "FAIL", test.name);
		if (gFailuresInTest != 0)
		{
			++failedTests;
		}
	}
	const int total = static_cast<int>(Registry().size());
	std::printf("\n%d tests, %d passed, %d failed, %d checks\n", total, total - failedTests, failedTests, gChecks);
	return failedTests == 0 ? 0 : 1;
}
