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
#include <optional>
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
[[maybe_unused]] std::string ToText(DeferredLinkStatus value)
{
	static const char* const kNames[] = {"Disabled", "Checking", "Pending", "NotFirstLaunch", "Unavailable", "NoReferrer",
		"NotFound", "Superseded", "Delivered", "Failed", "AwaitingPaste"};
	return kNames[static_cast<int>(value)];
}
[[maybe_unused]] std::string ToText(PastedLinkResult value)
{
	static const char* const kNames[] = {"Delivered", "NotOurs", "NotFound", "AlreadyHandled", "Disabled", "Failed", "<none>"};
	return kNames[static_cast<int>(value)];
}

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
	/** Requests to the deferred-link endpoints (redeem and match); answered with 404 by default. */
	std::function<Reply(const HttpRequest&)> onDeferred;
	std::vector<HttpRequest> deferred;
	std::vector<std::uint64_t> deferredIds;

	std::uint64_t Send(const HttpRequest& request, Callback onDone) override
	{
		const std::uint64_t id = nextId_++;
		const std::string eventsSuffix = "/v1/public/events";
		const bool isEvent = request.url.size() >= eventsSuffix.size() &&
			request.url.compare(request.url.size() - eventsSuffix.size(), eventsSuffix.size(), eventsSuffix) == 0;
		const std::string linksSuffix = "/v1/public/links";
		const bool isCreate = request.url.size() >= linksSuffix.size() &&
			request.url.compare(request.url.size() - linksSuffix.size(), linksSuffix.size(), linksSuffix) == 0;
		const bool isDeferred = request.url.find("/v1/public/deferred-links/") != std::string::npos;
		Reply reply;
		if (isDeferred)
		{
			deferred.push_back(request);
			deferredIds.push_back(id);
			reply = onDeferred ? onDeferred(request) : Respond(404);
		}
		else if (isEvent)
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
	std::vector<std::string> allowedHosts = {kHost};
	/** Unset: the option is not set, as in every test before links without an API call. */
	std::optional<ParameterLinks> parameterLinks;
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
		config.allowedHosts = options.allowedHosts;
		config.parameterLinks = options.parameterLinks;
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
	EXPECT_EQ(StringMember(event, "sdkVersion"), "0.4.0");
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
	EXPECT_EQ(BuildAppOpenBody("id-1", "link_1", "android", "0.3.0"),
		"{\"eventId\":\"id-1\",\"linkId\":\"link_1\",\"type\":\"app_open\",\"platform\":\"android\",\"sdkVersion\":\"0.3.0\"}");

	json::Value event;
	EXPECT(json::Parse(BuildAppOpenBody("id", "li\"nk\\\n", "unknown", std::string(40, 'v')), event));
	EXPECT_EQ(StringMember(event, "linkId"), "li\"nk\\\n");
	EXPECT_EQ(StringMember(event, "sdkVersion"), std::string(32, 'v'));
	EXPECT_EQ(event.members.size(), static_cast<std::size_t>(5));

	EXPECT(json::Parse(BuildAppOpenBody("id", "l", "ios", ""), event));
	EXPECT_EQ(StringMember(event, "sdkVersion"), std::string(kSdkVersion));
	EXPECT_EQ(std::string(kSdkVersion), "0.4.0");
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
// Campaign values ("Campaign query" in the SDK contract, decision 0018)
// ---------------------------------------------------------------------------------------------

namespace
{

/** The lookup URL for `url`, or "<not ours>". */
std::string LookupOf(const std::string& url)
{
	std::string lookup;
	return BuildLookupUrl(url, Hosts(), lookup) ? lookup : std::string("<not ours>");
}

std::string ResolvedBody(const std::string& campaign)
{
	return "{\"linkId\":\"link_1\",\"appId\":\"app_1\",\"routePath\":\"/x\"" +
		(campaign.empty() ? std::string() : ",\"campaign\":" + campaign) + "}";
}

std::string Repeat(const std::string& text, int times)
{
	std::string result;
	for (int index = 0; index < times; ++index)
	{
		result += text;
	}
	return result;
}

} // namespace

TEST(Campaign, TheExampleOfTheContract)
{
	std::string lookup;
	EXPECT(BuildLookupUrl(
		"https://MyApp.strybok.link/invite777?utm_source=instagram&fbclid=abc&utm_medium=two%20words&utm_source=x#top",
		NormalizeHosts({"myapp.strybok.link"}), lookup));
	EXPECT_EQ(lookup, "https://myapp.strybok.link/invite777?utm_source=instagram");
}

TEST(Campaign, WithoutCampaignParametersTheLookupIsWhatEarlierVersionsSent)
{
	const std::vector<std::string> suffixes = {
		"",
		"?",
		"#utm_source=fragment",
		"?#utm_source=fragment",
		"?token=secret&fbclid=abc&gclid=1&utm_id=7#f",
		"?a=1#?utm_source=fragment",
		"?utm_source=",
		"?utm_source",
		"?&&=&=x&",
	};
	for (const std::string& suffix : suffixes)
	{
		EXPECT_MSG(LookupOf(kInvite + suffix) == kInvite, suffix);
	}
	EXPECT_EQ(BuildCampaignQuery(""), "");
	EXPECT_EQ(BuildCampaignQuery("&"), "");
	EXPECT_EQ(BuildCampaignQuery("="), "");
	EXPECT_EQ(BuildCampaignQuery("&=&&"), "");
}

TEST(Campaign, KeepsTheFiveKeysInTheFixedOrderAndNothingElse)
{
	EXPECT_EQ(
		LookupOf(kInvite + "?utm_content=n&x=1&utm_term=t&utm_campaign=c&token=secret&utm_medium=m&utm_source=s#utm_source=f"),
		kInvite + "?utm_source=s&utm_medium=m&utm_campaign=c&utm_term=t&utm_content=n");
	EXPECT_EQ(LookupOf(kInvite + "?utm_content=n&&utm_medium=m&"), kInvite + "?utm_medium=m&utm_content=n");
	EXPECT_EQ(LookupOf(kInvite + "?utm_source=a#b"), kInvite + "?utm_source=a");
	EXPECT_EQ(BuildCampaignQuery("utm_term=t"), "?utm_term=t");
}

TEST(Campaign, KeyNamesAreExactAndLowerCase)
{
	const std::vector<std::string> queries = {
		"UTM_SOURCE=a",
		"Utm_Source=a",
		"utm_sourcex=a",
		"xutm_source=a",
		"utm_id=a",
		"utm_source%20=a",
		"+utm_source=a",
		" utm_source=a",
		"utm_source[]=a",
		std::string("utm_source\0=a", 13),
	};
	for (const std::string& query : queries)
	{
		EXPECT_MSG(BuildCampaignQuery(query).empty(), query);
	}
}

TEST(Campaign, TheFirstOccurrenceOfAKeyCountsEvenWhenItsValueIsDropped)
{
	EXPECT_EQ(BuildCampaignQuery("utm_source=first&utm_source=second"), "?utm_source=first");
	EXPECT_EQ(BuildCampaignQuery("utm_source=not%20valid&utm_source=second"), "");
	EXPECT_EQ(BuildCampaignQuery("utm_source=&utm_source=second"), "");
	EXPECT_EQ(BuildCampaignQuery("utm_source&utm_source=second"), "");
	EXPECT_EQ(BuildCampaignQuery("utm_source=a%2Fb&utm_medium=m&utm_source=second"), "?utm_medium=m");
}

TEST(Campaign, ValuesOutsideTheAllowedCharactersOrLengthAreDropped)
{
	const std::vector<std::string> values = {
		"two%20words",
		"two+words",
		"two words",
		"a@b.c",
		"a%40b.c",
		"a/b",
		"a%2Fb",
		"a=b",
		"a%26utm_medium=b",
		"a%3Db",
		"(other)",
		"a,b",
		"a:b",
		"a*b",
		"a!b",
		"a'b",
		"a\"b",
		"<script>",
		"\xD0\xBA\xD0\xB0\xD0\xBC\xD0\xBF", // Cyrillic, raw UTF-8
		"%D0%BA%D0%B0",
		"caf%C3%A9",
		"a%00b",
		std::string("a\0b", 3),
		"a%0Ab",
		"a\nb",
		"a\x7F",
		std::string(65, 'a'),
	};
	for (const std::string& value : values)
	{
		EXPECT_MSG(BuildCampaignQuery("utm_source=" + value).empty(), value);
	}
	const std::string longest(64, 'a');
	EXPECT_EQ(BuildCampaignQuery("utm_source=" + longest), "?utm_source=" + longest);
	EXPECT_EQ(BuildCampaignQuery("utm_source=AZaz09._~-"), "?utm_source=AZaz09._~-");
}

TEST(Campaign, PercentEncodedKeysAndValuesAreDecoded)
{
	EXPECT_EQ(BuildCampaignQuery("utm_source=%69nsta%2Dgram"), "?utm_source=insta-gram");
	EXPECT_EQ(BuildCampaignQuery("utm_source=a%2e%5f%7Eb"), "?utm_source=a._~b");
	EXPECT_EQ(BuildCampaignQuery("utm%5Fsource=x"), "?utm_source=x");
	EXPECT_EQ(BuildCampaignQuery("%75tm_medium=x"), "?utm_medium=x");
	// 64 characters after decoding.
	const std::string encoded = Repeat("%61", 64);
	EXPECT_EQ(BuildCampaignQuery("utm_term=" + encoded), "?utm_term=" + std::string(64, 'a'));
	EXPECT_EQ(BuildCampaignQuery("utm_term=" + encoded + "%61"), "");
}

TEST(Campaign, MalformedEscapesAndInvalidUtf8DropTheValueAndNeverCrash)
{
	const std::vector<std::string> values = {
		"%",
		"a%",
		"a%2",
		"%2",
		"%zz",
		"%G0",
		"%0G",
		"a%-1",
		"%+1",
		"%%",
		"%%%",
		"%u0041",
		"%C3%28",       // invalid continuation byte
		"%FF",          // never valid in UTF-8
		"%C0%AE",       // overlong "."
		"%E2%82",       // truncated sequence
		"%ED%A0%80",    // encoded surrogate
		"%F4%90%80%80", // beyond U+10FFFF
		"\xC3\x28",     // the same, as raw bytes
		"\xFF",
		"\xC0\xAE",
		"\xE2\x82",
		"\x80",
		"a\xFF\xFE",
	};
	for (const std::string& value : values)
	{
		EXPECT_MSG(BuildCampaignQuery("utm_source=" + value).empty(), value);
		EXPECT_MSG(BuildCampaignQuery("utm_source=" + value + "&utm_medium=ok") == "?utm_medium=ok", value);
		EXPECT_MSG(BuildCampaignQuery(value + "=x&utm_medium=ok") == "?utm_medium=ok", value);
		EXPECT_MSG(LookupOf(kInvite + "?utm_source=" + value) == kInvite, value);
	}
	// An escape must not read past the end of its own parameter.
	EXPECT_EQ(BuildCampaignQuery("utm_source=a%4&1=x"), "");
	EXPECT_EQ(BuildCampaignQuery("utm_source=%&41"), "");
	EXPECT_EQ(BuildCampaignQuery("utm_source%3"), "");
	EXPECT_EQ(BuildCampaignQuery("%"), "");
	EXPECT_EQ(BuildCampaignQuery("%4"), "");
}

TEST(Campaign, AQueryChangesNeitherWhichUrlsAreOursNorTheHostAndPathThatAreUsed)
{
	const std::vector<std::string> hosts = Hosts();
	const std::string query = "?utm_source=a&utm_medium=b";
	const std::vector<std::string> foreign = {
		"http://my-app.strybok.link/invite777",
		"https://evil.example.com/invite777",
		"https://user:pw@my-app.strybok.link/invite777",
		"https://user@my-app.strybok.link/invite777",
		"https://my-app.strybok.link:8443/invite777",
		"https://my-app.strybok.link/a/b",
		"https://my-app.strybok.link/invite777/",
		"https://my-app.strybok.link/",
		"https://my-app.strybok.link",
		"https://my-app.strybok.link/" + std::string(65, 'a'),
	};
	for (const std::string& url : foreign)
	{
		EXPECT_MSG(!IsStrybokUrl(url, hosts), url);
		EXPECT_MSG(!IsStrybokUrl(url + query, hosts), url + query);
		EXPECT_MSG(LookupOf(url + query) == "<not ours>", url + query);
	}
	// Nothing in a query can stand in for the authority or the path.
	const std::vector<std::string> tricky = {
		"https://evil.example.com/x?utm_source=https://my-app.strybok.link/invite777",
		"https://evil.example.com?utm_source=a@my-app.strybok.link/invite777",
		"https://my-app.strybok.link?utm_source=/invite777",
		"https://my-app.strybok.link/?utm_source=a",
	};
	for (const std::string& url : tricky)
	{
		EXPECT_MSG(!IsStrybokUrl(url, hosts), url);
	}
	// Accepted with and without a query alike, and host and slug are the same either way.
	EXPECT(IsStrybokUrl(kInvite, hosts));
	EXPECT(IsStrybokUrl(kInvite + query, hosts));
	EXPECT_EQ(LookupOf("HTTPS://My-App.STRYBOK.link:443/Invite777?utm_source=a&token=s#f"),
		"https://my-app.strybok.link/Invite777?utm_source=a");
}

TEST(Campaign, ResolveSendsTheCampaignQueryAndNothingElseOfTheQuery)
{
	Bridge bridge;
	const std::string url =
		"https://My-App.Strybok.Link/invite777?token=secret&utm_medium=social&utm_source=instagram&email=a%40b.c#private";
	const Bridge::ResolveOutcome outcome = bridge.Resolve(url);
	EXPECT(outcome.ok);
	EXPECT_EQ(outcome.link.uri, url);

	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(1));
	const std::string& sent = bridge.api.resolves[0].url;
	EXPECT_EQ(sent, kResolvePrefix +
		"https%3A%2F%2Fmy-app.strybok.link%2Finvite777%3Futm_source%3Dinstagram%26utm_medium%3Dsocial");
	for (const char* secret : {"secret", "token", "email", "private", "b.c"})
	{
		EXPECT_MSG(sent.find(secret) == std::string::npos, secret);
	}
}

TEST(Campaign, AUrlWithoutCampaignParametersIsRequestedExactlyAsBefore)
{
	Bridge bridge;
	EXPECT(bridge.Resolve(kInvite).ok);
	EXPECT(bridge.Resolve(kInvite + "?token=secret&utm_source=two%20words#f").ok);
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(2));
	const std::string expected = kResolvePrefix + "https%3A%2F%2Fmy-app.strybok.link%2Finvite777";
	EXPECT_EQ(bridge.api.resolves[0].url, expected);
	EXPECT_EQ(bridge.api.resolves[1].url, expected);
}

TEST(Campaign, ResolvedLinkHoldsTheFiveStringFieldsTheServerSent)
{
	ResolvedLink link;
	EXPECT(ParseResolveBody(kInvite, ResolvedBody(
		"{\"source\":\"instagram\",\"medium\":\"social\",\"campaign\":\"spring\",\"term\":\"shoes\",\"content\":\"banner\",\"extra\":\"x\"}"),
		link));
	EXPECT_EQ(link.campaign.source, "instagram");
	EXPECT_EQ(link.campaign.medium, "social");
	EXPECT_EQ(link.campaign.campaign, "spring");
	EXPECT_EQ(link.campaign.term, "shoes");
	EXPECT_EQ(link.campaign.content, "banner");
	EXPECT(!link.campaign.IsEmpty());

	// A later answer without values does not keep the earlier ones.
	EXPECT(ParseResolveBody(kInvite, ResolvedBody(""), link));
	EXPECT(link.campaign.IsEmpty());
}

TEST(Campaign, IsEmptyWhenMissingOrNotAnObjectAndOddFieldsAreIgnored)
{
	const std::vector<std::string> campaigns = {
		"", "null", "[]", "[\"a\"]", "\"instagram\"", "7", "true", "{}", "{\"other\":\"x\"}", "{\"source\":\"\"}",
	};
	for (const std::string& campaign : campaigns)
	{
		ResolvedLink link;
		EXPECT_MSG(ParseResolveBody(kInvite, ResolvedBody(campaign), link), campaign);
		EXPECT_MSG(link.campaign.IsEmpty(), campaign);
		EXPECT_EQ(link.routePath, "/x");
	}

	ResolvedLink link;
	EXPECT(ParseResolveBody(kInvite, ResolvedBody(
		"{\"source\":7,\"medium\":null,\"campaign\":{\"a\":1},\"term\":[\"x\"],\"content\":\"banner\",\"utm_source\":\"x\"}"),
		link));
	EXPECT_EQ(link.campaign.source, "");
	EXPECT_EQ(link.campaign.medium, "");
	EXPECT_EQ(link.campaign.campaign, "");
	EXPECT_EQ(link.campaign.term, "");
	EXPECT_EQ(link.campaign.content, "banner");

	// The same through the response classification: never an error.
	HttpResponse response;
	response.transportOk = true;
	response.statusCode = 200;
	response.body = ResolvedBody("[1,2]");
	const AttemptResult result = ClassifyResolveResponse(kInvite, response);
	EXPECT(result.action == AttemptAction::Deliver);
	EXPECT(result.link.campaign.IsEmpty());
}

TEST(Campaign, AppOpenReportsTheCampaignExactlyAsResolveReturnedIt)
{
	Bridge bridge;
	// The server answers with other values than the URL has: the answer is what is reported.
	bridge.api.onResolve = [](const HttpRequest&) {
		return Respond(200, ResolvedBody("{\"source\":\"newsletter\",\"content\":\"footer\",\"ignored\":\"x\",\"term\":5}"));
	};
	bridge.Listen();
	EXPECT(bridge.Initialize());
	bridge.sdk->HandleUrl(kInvite + "?utm_source=instagram&utm_medium=social");
	bridge.loop.Settle();

	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(bridge.received[0].campaign.source, "newsletter");
	EXPECT_EQ(bridge.received[0].campaign.content, "footer");
	EXPECT_EQ(bridge.received[0].campaign.medium, "");
	EXPECT_EQ(bridge.received[0].campaign.term, "");

	EXPECT_EQ(bridge.api.events.size(), static_cast<std::size_t>(1));
	json::Value event;
	EXPECT(json::Parse(bridge.api.events[0].body, event));
	EXPECT_EQ(event.members.size(), static_cast<std::size_t>(6));
	const json::Value* campaign = event.Find("campaign");
	EXPECT(campaign != nullptr && campaign->type == json::Type::Object);
	if (campaign != nullptr)
	{
		EXPECT_EQ(campaign->members.size(), static_cast<std::size_t>(2));
		EXPECT_EQ(StringMember(*campaign, "source"), "newsletter");
		EXPECT_EQ(StringMember(*campaign, "content"), "footer");
	}
	const std::string& body = bridge.api.events[0].body;
	const std::string tail = ",\"campaign\":{\"source\":\"newsletter\",\"content\":\"footer\"}}";
	EXPECT(body.size() > tail.size() && body.compare(body.size() - tail.size(), tail.size(), tail) == 0);
}

TEST(Campaign, AQueuedLinkReportsItsCampaignToo)
{
	Bridge bridge;
	bridge.api.onResolve = [](const HttpRequest&) { return Respond(200, ResolvedBody("{\"medium\":\"email\"}")); };
	EXPECT(bridge.Initialize(kInvite)); // nobody listens: the link is queued
	EXPECT_EQ(bridge.api.events.size(), static_cast<std::size_t>(1));
	json::Value event;
	EXPECT(json::Parse(bridge.api.events[0].body, event));
	const json::Value* campaign = event.Find("campaign");
	EXPECT(campaign != nullptr);
	if (campaign != nullptr)
	{
		EXPECT_EQ(StringMember(*campaign, "medium"), "email");
	}
	bridge.Listen();
	bridge.loop.Settle();
	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(1));
	EXPECT_EQ(bridge.received[0].campaign.medium, "email");
}

TEST(Campaign, AppOpenHasNoCampaignWhenResolveReturnedNoneWhateverTheUrlSays)
{
	const std::vector<std::string> campaigns = {"", "{}", "null", "[]", "{\"source\":1}"};
	for (const std::string& campaign : campaigns)
	{
		Bridge bridge;
		bridge.api.onResolve = [campaign](const HttpRequest&) { return Respond(200, ResolvedBody(campaign)); };
		bridge.Listen();
		EXPECT(bridge.Initialize());
		bridge.sdk->HandleUrl(kInvite + "?utm_source=instagram");
		bridge.loop.Settle();

		EXPECT_EQ(bridge.api.events.size(), static_cast<std::size_t>(1));
		if (bridge.api.events.size() == 1)
		{
			json::Value event;
			EXPECT(json::Parse(bridge.api.events[0].body, event));
			EXPECT_MSG(event.members.size() == 5, campaign);
			EXPECT_MSG(event.Find("campaign") == nullptr, campaign);
			EXPECT_MSG(bridge.api.events[0].body.find("instagram") == std::string::npos, campaign);
		}
	}
}

TEST(Campaign, AppOpenBodyIsExactWithAndWithoutCampaign)
{
	const std::string base =
		"{\"eventId\":\"id-1\",\"linkId\":\"link_1\",\"type\":\"app_open\",\"platform\":\"android\",\"sdkVersion\":\"1.2.3\"";
	EXPECT_EQ(BuildAppOpenBody("id-1", "link_1", "android", "1.2.3"), base + "}");
	EXPECT_EQ(BuildAppOpenBody("id-1", "link_1", "android", "1.2.3", Campaign()), base + "}");

	Campaign all;
	all.source = "s";
	all.medium = "m";
	all.campaign = "c";
	all.term = "t";
	all.content = "n";
	EXPECT_EQ(BuildAppOpenBody("id-1", "link_1", "android", "1.2.3", all),
		base + ",\"campaign\":{\"source\":\"s\",\"medium\":\"m\",\"campaign\":\"c\",\"term\":\"t\",\"content\":\"n\"}}");

	Campaign one;
	one.term = "t\"x";
	EXPECT_EQ(BuildAppOpenBody("id-1", "link_1", "android", "1.2.3", one), base + ",\"campaign\":{\"term\":\"t\\\"x\"}}");
}

TEST(Campaign, TheSameSlugWithDifferentCampaignParametersIsTwoLinks)
{
	Bridge bridge;
	bridge.Listen();
	EXPECT(bridge.Initialize());

	bridge.sdk->HandleUrl(kInvite + "?utm_source=a");
	bridge.sdk->HandleUrl(kInvite + "?utm_source=a"); // the same URL string: one tap reported twice
	bridge.sdk->HandleUrl(kInvite + "?utm_source=b");
	bridge.sdk->HandleUrl(kInvite);
	// Compared as received: a parameter that is never sent still makes another URL.
	bridge.sdk->HandleUrl(kInvite + "?fbclid=1");
	bridge.sdk->HandleUrl(kInvite + "?fbclid=1");
	bridge.loop.Settle();

	EXPECT_EQ(bridge.received.size(), static_cast<std::size_t>(4));
	EXPECT_EQ(bridge.api.resolves.size(), static_cast<std::size_t>(4));
	if (bridge.api.resolves.size() == 4 && bridge.received.size() == 4)
	{
		const std::string plain = kResolvePrefix + "https%3A%2F%2Fmy-app.strybok.link%2Finvite777";
		EXPECT_EQ(bridge.api.resolves[0].url, plain + "%3Futm_source%3Da");
		EXPECT_EQ(bridge.api.resolves[1].url, plain + "%3Futm_source%3Db");
		EXPECT_EQ(bridge.api.resolves[2].url, plain);
		EXPECT_EQ(bridge.api.resolves[3].url, plain);
		EXPECT_EQ(bridge.received[0].uri, kInvite + "?utm_source=a");
		EXPECT_EQ(bridge.received[1].uri, kInvite + "?utm_source=b");
		EXPECT_EQ(bridge.received[2].uri, kInvite);
		EXPECT_EQ(bridge.received[3].uri, kInvite + "?fbclid=1");
	}
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
// deferred links (SDK contract, "Deferred links (optional, Android)" and "Deferred links on iOS")
// ---------------------------------------------------------------------------------------------

namespace
{

const std::string kClick = "dc_0123456789abcdefghijABCDEFGHIJ01";
const std::string kOtherClick = "dc_ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ";
const std::string kRedeemUrl = "https://api.strybok.com/v1/public/deferred-links/redeem";
const std::string kMatchUrl = "https://api.strybok.com/v1/public/deferred-links/match";
const std::string kPackage = "com.example.game";
const std::string kBundle = "com.example.Game";
const std::string kPasted = "https://my-app.strybok.link/invite777?strybok_click=" + kClick;

/** Survives a "restart": the same store is handed to the next engine. */
class MemoryStore final : public IDeferredLinkStore
{
public:
	std::map<std::string, std::string> values;
	/** Every write, as `key=value`, in order. */
	std::vector<std::string> writes;
	int reads = 0;
	bool failReads = false;
	/** Writes to this key fail. */
	std::string failWritesTo;

	bool Read(const std::string& key, std::string& outValue, bool& outFound) override
	{
		++reads;
		if (failReads)
		{
			return false;
		}
		const auto found = values.find(key);
		outFound = found != values.end();
		outValue = outFound ? found->second : std::string();
		return true;
	}

	bool Write(const std::string& key, const std::string& value) override
	{
		if (key == failWritesTo)
		{
			return false;
		}
		values[key] = value;
		writes.push_back(key + "=" + value);
		return true;
	}

	std::string Get(const std::string& key) const
	{
		const auto found = values.find(key);
		return found == values.end() ? std::string("<none>") : found->second;
	}

	std::string Attempts() const { return Get(kDeferredAttemptsKey); }
	bool Done() const { return Get(kDeferredDoneKey) == "1"; }
};

InstallReferrerAnswer Referrer(const std::string& referrer)
{
	InstallReferrerAnswer answer;
	answer.kind = InstallReferrerAnswer::Kind::Referrer;
	answer.referrer = referrer;
	return answer;
}

InstallReferrerAnswer ForNow()
{
	InstallReferrerAnswer answer;
	answer.kind = InstallReferrerAnswer::Kind::UnavailableForNow;
	return answer;
}

InstallReferrerAnswer Never()
{
	InstallReferrerAnswer answer;
	answer.kind = InstallReferrerAnswer::Kind::Unavailable;
	return answer;
}

/** A source that answers before returning. */
InstallReferrerSource Answering(const InstallReferrerAnswer& answer, int* calls = nullptr)
{
	return [answer, calls](std::function<void(const InstallReferrerAnswer&)> done) {
		if (calls != nullptr)
		{
			++*calls;
		}
		done(answer);
	};
}

/** A source that keeps the question until the test answers it. */
struct HeldSource
{
	std::shared_ptr<std::vector<std::function<void(const InstallReferrerAnswer&)>>> questions =
		std::make_shared<std::vector<std::function<void(const InstallReferrerAnswer&)>>>();

	InstallReferrerSource Source() const
	{
		const auto held = questions;
		return [held](std::function<void(const InstallReferrerAnswer&)> done) { held->push_back(std::move(done)); };
	}

	std::size_t Asked() const { return questions->size(); }
	void Answer(const InstallReferrerAnswer& answer) const { questions->back()(answer); }
};

std::string DeferredBody(
	const std::string& url = "https://my-app.strybok.link/invite777", const std::string& extra = std::string())
{
	return "{\"linkId\":\"link_d\",\"appId\":\"app_1\",\"routePath\":\"/invite/abc123\","
		"\"payload\":{\"invitationId\":\"abc123\"},\"campaign\":{\"source\":\"instagram\"},"
		"\"url\":\"" + url + "\",\"deferred\":true" + extra + "}";
}

std::string MatchedBody()
{
	return DeferredBody("https://my-app.strybok.link/invite777", ",\"matched\":\"probabilistic\"");
}

std::string HeaderOf(const HttpRequest& request, const std::string& name)
{
	for (const auto& header : request.headers)
	{
		if (header.first == name)
		{
			return header.second;
		}
	}
	return "<missing>";
}

/** An engine with deferred links switched on, wired to fakes. `config` may be changed before `Make`. */
struct Deferred
{
	FakeLoop loop;
	FakeApi api{loop};
	std::shared_ptr<MemoryStore> store;
	Config config;
	std::vector<ResolvedLink> received;
	std::vector<LinkError> errors;
	std::vector<DeferredLinkStatus> statuses;
	std::vector<PastedLinkResult> pasted;
	std::unique_ptr<LinkEngine> sdk;

	explicit Deferred(const std::string& platform = "android", std::shared_ptr<MemoryStore> existing = nullptr)
		: store(existing ? std::move(existing) : std::make_shared<MemoryStore>())
	{
		config.apiBaseUrl = kApi;
		config.allowedHosts = {kHost};
		config.timeoutMs = 60;
		config.maxRetries = 2;
		config.retryDelayMs = 1;
		config.platform = platform;
		config.deferredLinks = true;
		config.androidPackageName = kPackage;
		config.iosBundleId = kBundle;
		config.deferredLinkStore = store;
		config.deviceFamily = []() { return std::string("iphone"); };
		config.locale = []() { return std::string("en-GB"); };
	}

	LinkEngine& Make()
	{
		sdk.reset(new LinkEngine(config, api, loop, loop));
		sdk->SetErrorHandler([this](const LinkError& error) { errors.push_back(error); });
		sdk->SetDeferredLinkStatusHandler([this](DeferredLinkStatus status) { statuses.push_back(status); });
		return *sdk;
	}

	void Listen()
	{
		if (!sdk)
		{
			Make();
		}
		sdk->SetLinkHandler([this](const ResolvedLink& link) { received.push_back(link); });
		sdk->SetListening(true);
	}

	/** Makes the engine if needed, starts it and lets the check run. Returns whether start became ready. */
	bool Start(const std::string& launchUrl = std::string())
	{
		if (!sdk)
		{
			Make();
		}
		bool ready = false;
		sdk->Start(launchUrl.empty() ? nullptr : &launchUrl, [&ready]() { ready = true; });
		loop.Settle();
		return ready;
	}

	void Paste(const std::string& text)
	{
		sdk->RedeemPastedLink(text, [this](PastedLinkResult result) { pasted.push_back(result); });
	}

	DeferredLinkStatus Status() const { return sdk->GetDeferredLinkStatus(); }
};

std::size_t Count(std::size_t value) { return value; }

} // namespace

TEST(DeferredParsing, FindsTheClickIdInAReferrer)
{
	std::string id;
	EXPECT(FindClickIdInReferrer("strybok_click=" + kClick, id));
	EXPECT_EQ(id, kClick);
	EXPECT(FindClickIdInReferrer("utm_source=x&strybok_click=" + kClick + "&utm_medium=y", id));
	EXPECT_EQ(id, kClick);
	// The first part with the key decides, also when its value is unusable.
	EXPECT(FindClickIdInReferrer("strybok_click=" + kClick + "&strybok_click=" + kOtherClick, id));
	EXPECT_EQ(id, kClick);
	EXPECT(!FindClickIdInReferrer("strybok_click=bad&strybok_click=" + kClick, id));
	EXPECT(!FindClickIdInReferrer("strybok_click&strybok_click=" + kClick, id));

	EXPECT(!FindClickIdInReferrer("", id));
	EXPECT(!FindClickIdInReferrer("utm_source=google-play&utm_medium=organic", id));
	EXPECT(!FindClickIdInReferrer("strybok_click=", id));
	EXPECT(!FindClickIdInReferrer("strybok_click=dc_short", id));
	EXPECT(!FindClickIdInReferrer("strybok_click=" + kClick + "x", id));
	EXPECT(!FindClickIdInReferrer("strybok_click=xc_0123456789abcdefghijABCDEFGHIJ01", id));
	EXPECT(!FindClickIdInReferrer("strybok_click=dc_0123456789abcdefghijABCDEFGHIJ0-", id));
	EXPECT(!FindClickIdInReferrer("strybok_click=dc_0123456789abcdefghijABCDEFGHIJ0%31", id));
	// Other keys that merely contain ours.
	EXPECT(!FindClickIdInReferrer("xstrybok_click=" + kClick, id));
	EXPECT(!FindClickIdInReferrer("strybok_clicks=" + kClick, id));
	EXPECT(!FindClickIdInReferrer("STRYBOK_CLICK=" + kClick, id));
	EXPECT(!FindClickIdInReferrer("x=strybok_click=" + kClick, id));
	EXPECT_EQ(id, "");
}

TEST(DeferredParsing, DecodesAStillEncodedReferrerOnce)
{
	std::string id;
	EXPECT(FindClickIdInReferrer("strybok_click%3D" + kClick, id));
	EXPECT_EQ(id, kClick);
	EXPECT(FindClickIdInReferrer("strybok_click%3d" + kClick + "%26utm_source%3Dx", id));
	EXPECT_EQ(id, kClick);
	EXPECT(FindClickIdInReferrer("utm_source%3Dx%26STRYBOK_click%3D" + kClick, id) == false); // decoded key is not ours
	EXPECT(FindClickIdInReferrer("utm_source%3Dx%26strybok_click%3D" + kClick, id));
	EXPECT_EQ(id, kClick);
	// A malformed escape anywhere: no id, never an error.
	EXPECT(!FindClickIdInReferrer("strybok_click%3D" + kClick + "%2", id));
	EXPECT(!FindClickIdInReferrer("strybok_click%3D" + kClick + "%zz", id));
	// Decoded once only.
	EXPECT(!FindClickIdInReferrer("strybok_click%253D" + kClick, id));
	// No fallback when a part already has the key.
	EXPECT(!FindClickIdInReferrer("strybok_click=bad&x=strybok_click%3D" + kClick, id));
	// The decoded value must match as well.
	EXPECT(!FindClickIdInReferrer("strybok_click%3Dnope", id));
}

TEST(DeferredParsing, FindsTheClickIdInAPastedLink)
{
	std::string id;
	EXPECT(FindClickIdInPastedLink(kPasted, Hosts(), id));
	EXPECT_EQ(id, kClick);
	EXPECT(FindClickIdInPastedLink("  \n" + kPasted + "\t ", Hosts(), id));
	EXPECT(FindClickIdInPastedLink("HTTPS://MY-APP.strybok.link/invite777?utm_source=a&strybok_click=" + kClick + "#top", Hosts(), id));
	EXPECT_EQ(id, kClick);
	// Standard query decoding of the key.
	EXPECT(FindClickIdInPastedLink("https://my-app.strybok.link/invite777?strybok%5Fclick=" + kClick, Hosts(), id));
	// The first occurrence decides.
	EXPECT(FindClickIdInPastedLink(kPasted + "&strybok_click=" + kOtherClick, Hosts(), id));
	EXPECT_EQ(id, kClick);
	EXPECT(!FindClickIdInPastedLink("https://my-app.strybok.link/invite777?strybok_click=bad&strybok_click=" + kClick, Hosts(), id));

	EXPECT(!FindClickIdInPastedLink("", Hosts(), id));
	EXPECT(!FindClickIdInPastedLink("hello world", Hosts(), id));
	EXPECT(!FindClickIdInPastedLink("https://my-app.strybok.link/invite777", Hosts(), id));
	EXPECT(!FindClickIdInPastedLink("https://my-app.strybok.link/invite777#strybok_click=" + kClick, Hosts(), id));
	EXPECT(!FindClickIdInPastedLink("https://evil.example/invite777?strybok_click=" + kClick, Hosts(), id));
	EXPECT(!FindClickIdInPastedLink("http://my-app.strybok.link/invite777?strybok_click=" + kClick, Hosts(), id));
	EXPECT(!FindClickIdInPastedLink("https://my-app.strybok.link/a/b?strybok_click=" + kClick, Hosts(), id));
	EXPECT(!FindClickIdInPastedLink("https://user@my-app.strybok.link/invite777?strybok_click=" + kClick, Hosts(), id));
	EXPECT(!FindClickIdInPastedLink("https://my-app.strybok.link/invite777?strybok_click=dc_tooshort", Hosts(), id));
	EXPECT(!FindClickIdInPastedLink("text before " + kPasted, Hosts(), id));
	EXPECT_EQ(id, "");
}

TEST(DeferredParsing, RequestBodiesAndEndpointsAreExact)
{
	EXPECT_EQ(BuildDeferredRedeemEndpoint("https://api.strybok.com/"), kRedeemUrl);
	EXPECT_EQ(BuildDeferredMatchEndpoint("https://api.example/base//"), "https://api.example/base/v1/public/deferred-links/match");
	EXPECT_EQ(BuildAndroidRedeemBody(kClick, kPackage, "0.3.0"),
		"{\"platform\":\"android\",\"clickId\":\"" + kClick + "\",\"packageName\":\"com.example.game\",\"sdkVersion\":\"0.3.0\"}");
	EXPECT_EQ(BuildIosRedeemBody(kClick, kBundle, ""),
		"{\"platform\":\"ios\",\"clickId\":\"" + kClick + "\",\"bundleId\":\"com.example.Game\",\"sdkVersion\":\"" +
			std::string(kSdkVersion) + "\"}");
	EXPECT_EQ(BuildMatchBody(kBundle, "ipad", "uk-UA", "0.3.0+unreal"),
		"{\"platform\":\"ios\",\"bundleId\":\"com.example.Game\",\"deviceFamily\":\"ipad\",\"locale\":\"uk-UA\",\"sdkVersion\":\"0.3.0+unreal\"}");
}

TEST(DeferredParsing, DeviceFamilyAndLocaleAreWhatTheServerAccepts)
{
	EXPECT(IsMatchDeviceFamily("iphone"));
	EXPECT(IsMatchDeviceFamily("ipad"));
	EXPECT(IsMatchDeviceFamily("ipod"));
	EXPECT(!IsMatchDeviceFamily(""));
	EXPECT(!IsMatchDeviceFamily("iPhone"));
	EXPECT(!IsMatchDeviceFamily("mac"));

	for (const char* good : {"en", "eng", "en-GB", "en_GB", "zh-Hans-CN", "sr-Latn-RS-x-a1", "de-1996"})
	{
		EXPECT_MSG(IsMatchLocale(good), good);
	}
	for (const char* bad : {"", "e", "engl", "en-", "en--GB", "en GB", "en-GB ", "1n", "en-abcdefghi", "en-a-b-c-d-e", "en.GB", "-en"})
	{
		EXPECT_MSG(!IsMatchLocale(bad), bad);
	}
}

TEST(DeferredParsing, AnswerMustBeAResolvedDeferredLinkOfThisApp)
{
	ResolvedLink link;
	EXPECT(ParseDeferredBody(DeferredBody(), Hosts(), false, link));
	EXPECT_EQ(link.uri, "https://my-app.strybok.link/invite777");
	EXPECT_EQ(link.linkId, "link_d");
	EXPECT_EQ(link.routePath, "/invite/abc123");
	EXPECT_EQ(link.payloadJson, "{\"invitationId\":\"abc123\"}");
	EXPECT_EQ(link.campaign.source, "instagram");
	EXPECT(link.isDeferred);
	EXPECT(!link.isProbabilisticMatch);

	// Unknown fields are ignored; `matched` is only required for a match.
	EXPECT(ParseDeferredBody(MatchedBody(), Hosts(), false, link));
	EXPECT(ParseDeferredBody(MatchedBody(), Hosts(), true, link));
	EXPECT(link.isDeferred);
	EXPECT(link.isProbabilisticMatch);
	EXPECT(!ParseDeferredBody(DeferredBody(), Hosts(), true, link));
	EXPECT(!ParseDeferredBody(DeferredBody("https://my-app.strybok.link/invite777", ",\"matched\":\"exact\""), Hosts(), true, link));

	EXPECT(!ParseDeferredBody("not json", Hosts(), false, link));
	EXPECT(!ParseDeferredBody("[]", Hosts(), false, link));
	EXPECT(!ParseDeferredBody(OkBody(), Hosts(), false, link)); // a plain resolve answer
	EXPECT(!ParseDeferredBody(DeferredBody("https://evil.example/invite777"), Hosts(), false, link));
	EXPECT(!ParseDeferredBody(DeferredBody("https://my-app.strybok.link/a/b"), Hosts(), false, link));
	EXPECT(!ParseDeferredBody(DeferredBody("/invite777"), Hosts(), false, link));
	const std::string url = "\"url\":\"https://my-app.strybok.link/invite777\"";
	EXPECT(!ParseDeferredBody("{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"/x\"," + url + "}", Hosts(), false, link));
	EXPECT(!ParseDeferredBody("{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"/x\"," + url + ",\"deferred\":false}", Hosts(), false, link));
	EXPECT(!ParseDeferredBody("{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"/x\"," + url + ",\"deferred\":\"true\"}", Hosts(), false, link));
	EXPECT(!ParseDeferredBody("{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"/x\",\"url\":7,\"deferred\":true}", Hosts(), false, link));
	EXPECT(!ParseDeferredBody("{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"//x\"," + url + ",\"deferred\":true}", Hosts(), false, link));
	EXPECT(!ParseDeferredBody("{\"linkId\":\"l\",\"routePath\":\"/x\"," + url + ",\"deferred\":true}", Hosts(), false, link));
	EXPECT(!ParseDeferredBody("{\"linkId\":7,\"appId\":\"a\",\"routePath\":\"/x\"," + url + ",\"deferred\":true}", Hosts(), false, link));
}

// --- Android ---

TEST(DeferredAndroid, IsOffByDefaultAndThenDoesNothingAtAll)
{
	Deferred d;
	d.config.deferredLinks = false;
	int asked = 0;
	d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick), &asked)};
	d.Listen();
	EXPECT(Config().deferredLinks == false);
	EXPECT(Config().allowProbabilisticMatching == false);
	EXPECT(d.Start());
	d.loop.Advance(60000);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Disabled);
	EXPECT_EQ(asked, 0);
	EXPECT_EQ(d.api.deferred.size(), Count(0));
	EXPECT_EQ(d.store->reads, 0);
	EXPECT_EQ(d.store->writes.size(), Count(0));
	EXPECT_EQ(d.statuses.size(), Count(0));
	d.Paste(kPasted);
	EXPECT_EQ(d.pasted.size(), Count(1));
	EXPECT_EQ(d.pasted[0], PastedLinkResult::Disabled);
	EXPECT_EQ(d.api.deferred.size(), Count(0));
	// Normal links carry the two flags, false.
	d.sdk->HandleUrl(kInvite);
	d.loop.Settle();
	EXPECT_EQ(d.received.size(), Count(1));
	EXPECT(!d.received[0].isDeferred);
	EXPECT(!d.received[0].isProbabilisticMatch);
}

TEST(DeferredAndroid, StaysDisabledWithoutAStoreAnIdentifierOrAKnownPlatform)
{
	int asked = 0;
	const auto run = [&asked](Deferred& d) {
		d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick), &asked)};
		d.config.allowProbabilisticMatching = true;
		d.Listen();
		EXPECT(d.Start());
		d.sdk->EnableDeferredLinks();
		d.loop.Advance(60000);
		d.Paste(kPasted);
		d.sdk->SkipPastedLink();
		d.loop.Advance(60000);
		EXPECT_EQ(d.Status(), DeferredLinkStatus::Disabled);
		EXPECT_EQ(d.statuses.size(), Count(0));
		EXPECT_EQ(d.api.deferred.size(), Count(0));
		EXPECT_EQ(d.store->writes.size(), Count(0));
		EXPECT_EQ(d.pasted.size(), Count(1));
		EXPECT_EQ(d.pasted[0], PastedLinkResult::Disabled);
		EXPECT_EQ(d.received.size(), Count(0));
	};

	Deferred noStore;
	noStore.config.deferredLinkStore = nullptr;
	run(noStore);

	Deferred noPackage;
	noPackage.config.androidPackageName = "  ";
	run(noPackage);

	Deferred noBundle("ios");
	noBundle.config.iosBundleId = "";
	run(noBundle);

	Deferred unknown("unknown");
	run(unknown);

	EXPECT_EQ(asked, 0);
}

TEST(DeferredAndroid, StartCompletesWhileASourceBlocksAndAttemptsAreStoredFirst)
{
	Deferred d;
	std::string attemptsWhenAsked = "<not asked>";
	bool doneWhenAsked = true;
	const std::shared_ptr<MemoryStore> store = d.store;
	d.config.installReferrerSources = {[&attemptsWhenAsked, &doneWhenAsked, store](std::function<void(const InstallReferrerAnswer&)>) {
		attemptsWhenAsked = store->Attempts();
		doneWhenAsked = store->Done();
		// never answers
	}};
	d.Make();
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Pending);

	bool ready = false;
	d.sdk->Start(&kInvite, [&ready]() { ready = true; });
	// Nothing of the check happens inside Start.
	EXPECT_EQ(attemptsWhenAsked, "<not asked>");
	EXPECT_EQ(d.store->reads, 0);
	d.loop.Settle();
	EXPECT(ready);
	EXPECT_EQ(attemptsWhenAsked, "1");
	EXPECT(!doneWhenAsked);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Checking);

	// The launch link, GetInitialLink and Resolve do not wait for the source.
	ResolvedLink initial;
	EXPECT(d.sdk->TakeInitialLink(initial));
	EXPECT_EQ(initial.uri, kInvite);
	EXPECT(!initial.isDeferred);
	bool resolved = false;
	d.sdk->Resolve(kInvite, [&resolved](const ResolvedLink* link, const LinkError*) { resolved = link != nullptr; });
	d.loop.Settle();
	EXPECT(resolved);
	EXPECT_EQ(d.api.deferred.size(), Count(0));
}

TEST(DeferredAndroid, RedeemsTheClickIdAndDeliversTheLinkOnce)
{
	Deferred d;
	d.config.installReferrerSources = {Answering(Referrer("utm_source=google-play&strybok_click=" + kClick + "&utm_medium=organic"))};
	d.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
	d.Make();
	bool doneWhenDelivered = false;
	DeferredLinkStatus statusWhenDelivered = DeferredLinkStatus::Disabled;
	d.sdk->SetLinkHandler([&d, &doneWhenDelivered, &statusWhenDelivered](const ResolvedLink& link) {
		doneWhenDelivered = d.store->Done();
		statusWhenDelivered = d.Status();
		d.received.push_back(link);
	});
	d.sdk->SetListening(true);
	EXPECT(d.Start());

	EXPECT_EQ(d.api.deferred.size(), Count(1));
	const HttpRequest& request = d.api.deferred[0];
	EXPECT_EQ(request.method, "POST");
	EXPECT_EQ(request.url, kRedeemUrl);
	EXPECT_EQ(request.headers.size(), Count(2));
	EXPECT_EQ(HeaderOf(request, "Content-Type"), "application/json");
	EXPECT_EQ(HeaderOf(request, "Accept"), "application/json");
	EXPECT_EQ(request.body,
		"{\"platform\":\"android\",\"clickId\":\"" + kClick + "\",\"packageName\":\"com.example.game\",\"sdkVersion\":\"" +
			std::string(kSdkVersion) + "\"}");
	// Nothing else of the referrer leaves the device.
	EXPECT(request.body.find("utm_") == std::string::npos);
	EXPECT(request.body.find("google-play") == std::string::npos);

	EXPECT_EQ(d.received.size(), Count(1));
	const ResolvedLink& link = d.received[0];
	EXPECT_EQ(link.uri, "https://my-app.strybok.link/invite777");
	EXPECT_EQ(link.linkId, "link_d");
	EXPECT_EQ(link.appId, "app_1");
	EXPECT_EQ(link.routePath, "/invite/abc123");
	EXPECT_EQ(link.payloadJson, "{\"invitationId\":\"abc123\"}");
	EXPECT_EQ(link.campaign.source, "instagram");
	EXPECT(link.isDeferred);
	EXPECT(!link.isProbabilisticMatch);

	// `done` before delivery; the status says `Delivered`.
	EXPECT(doneWhenDelivered);
	EXPECT_EQ(statusWhenDelivered, DeferredLinkStatus::Delivered);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Delivered);
	EXPECT_EQ(d.statuses.size(), Count(2));
	EXPECT_EQ(d.statuses[0], DeferredLinkStatus::Checking);
	EXPECT_EQ(d.statuses[1], DeferredLinkStatus::Delivered);
	EXPECT_EQ(d.store->writes.size(), Count(2));
	EXPECT_EQ(d.store->writes[0], "strybok.deferred.attempts=1");
	EXPECT_EQ(d.store->writes[1], "strybok.deferred.done=1");

	// No app_open for a deferred link, nothing for onError, and nothing happens later.
	d.loop.Advance(60000);
	EXPECT_EQ(d.api.events.size(), Count(0));
	EXPECT_EQ(d.errors.size(), Count(0));
	EXPECT_EQ(d.api.deferred.size(), Count(1));
	EXPECT_EQ(d.received.size(), Count(1));
	EXPECT_EQ(d.api.resolves.size(), Count(0));
	// Only the two keys are ever stored.
	EXPECT_EQ(d.store->values.size(), Count(2));
}

TEST(DeferredAndroid, IsQueuedUntilAListenerAttachesAndIsNeverTheInitialLink)
{
	Deferred d;
	d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick))};
	d.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
	EXPECT(d.Start());
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Delivered);
	EXPECT_EQ(d.sdk->PendingCount(), Count(1));

	ResolvedLink initial;
	EXPECT(!d.sdk->TakeInitialLink(initial));
	bool called = false;
	bool hasLink = true;
	d.sdk->GetInitialLink([&called, &hasLink](const ResolvedLink* link) {
		called = true;
		hasLink = link != nullptr;
	});
	EXPECT(called);
	EXPECT(!hasLink);
	EXPECT_EQ(d.sdk->PendingCount(), Count(1)); // not removed from the queue either

	d.Listen();
	EXPECT_EQ(d.received.size(), Count(1));
	EXPECT(d.received[0].isDeferred);
	EXPECT_EQ(d.sdk->PendingCount(), Count(0));
	d.loop.Advance(60000);
	EXPECT_EQ(d.api.events.size(), Count(0));
}

TEST(DeferredAndroid, StatusFollowsWhatTheSourcesAnswer)
{
	struct Case
	{
		std::vector<InstallReferrerAnswer> answers;
		DeferredLinkStatus status;
		bool done;
	};
	const std::vector<Case> cases = {
		{{}, DeferredLinkStatus::Unavailable, true},
		{{Never()}, DeferredLinkStatus::Unavailable, true},
		{{Never(), Never()}, DeferredLinkStatus::Unavailable, true},
		{{Referrer("")}, DeferredLinkStatus::NoReferrer, true},
		{{Referrer("utm_source=google-play&utm_medium=organic")}, DeferredLinkStatus::NoReferrer, true},
		{{Never(), Referrer("strybok_click=broken")}, DeferredLinkStatus::NoReferrer, true},
		{{ForNow()}, DeferredLinkStatus::Pending, false},
		{{Referrer("x=y"), ForNow(), Never()}, DeferredLinkStatus::Pending, false},
		{{ForNow(), Referrer("x=y")}, DeferredLinkStatus::Pending, false},
	};
	for (const Case& testCase : cases)
	{
		Deferred d;
		int asked = 0;
		for (const InstallReferrerAnswer& answer : testCase.answers)
		{
			d.config.installReferrerSources.push_back(Answering(answer, &asked));
		}
		d.Listen();
		EXPECT(d.Start());
		EXPECT_EQ(d.Status(), testCase.status);
		EXPECT_EQ(d.store->Done(), testCase.done);
		EXPECT_EQ(d.store->Attempts(), "1");
		EXPECT_EQ(asked, static_cast<int>(testCase.answers.size()));
		EXPECT_EQ(d.api.deferred.size(), Count(0));
		EXPECT_EQ(d.received.size(), Count(0));
		EXPECT_EQ(d.errors.size(), Count(0));
	}
}

TEST(DeferredAndroid, AsksTheSourcesInOrderAndStopsAtTheFirstClickId)
{
	Deferred d;
	std::vector<int> order;
	const auto source = [&order](int number, const InstallReferrerAnswer& answer) -> InstallReferrerSource {
		return [&order, number, answer](std::function<void(const InstallReferrerAnswer&)> done) {
			order.push_back(number);
			done(answer);
		};
	};
	HeldSource second;
	d.config.installReferrerSources = {
		source(1, ForNow()),
		InstallReferrerSource(), // an empty function: skipped as "unavailable"
		second.Source(),
		source(3, Referrer("strybok_click=" + kClick)),
		source(4, Referrer("strybok_click=" + kOtherClick)),
	};
	d.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
	d.Listen();
	EXPECT(d.Start());
	// One after another: the third source is not asked while the second has not answered.
	EXPECT_EQ(order.size(), Count(1));
	EXPECT_EQ(second.Asked(), Count(1));
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Checking);
	second.Answer(Referrer("utm_source=galaxy"));
	d.loop.Settle();
	EXPECT_EQ(order.size(), Count(2));
	EXPECT_EQ(order[1], 3);
	EXPECT_EQ(d.api.deferred.size(), Count(1));
	EXPECT(d.api.deferred[0].body.find(kClick) != std::string::npos);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Delivered);
	EXPECT_EQ(d.received.size(), Count(1));
	// A second answer to an old question changes nothing.
	second.Answer(Referrer("strybok_click=" + kOtherClick));
	d.loop.Advance(60000);
	EXPECT_EQ(d.api.deferred.size(), Count(1));
	EXPECT_EQ(d.received.size(), Count(1));
}

TEST(DeferredAndroid, AStillEncodedReferrerIsRedeemed)
{
	Deferred d;
	d.config.installReferrerSources = {Answering(Referrer("strybok_click%3D" + kClick + "%26utm_source%3Dx"))};
	d.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
	d.Listen();
	EXPECT(d.Start());
	EXPECT_EQ(d.api.deferred.size(), Count(1));
	EXPECT(d.api.deferred[0].body.find("\"clickId\":\"" + kClick + "\"") != std::string::npos);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Delivered);
}

TEST(DeferredAndroid, ASourceThatTakesLongerThanTenSecondsIsUnavailableForNow)
{
	Deferred d;
	HeldSource slow;
	int askedNext = 0;
	d.config.installReferrerSources = {slow.Source(), Answering(Never(), &askedNext)};
	d.Listen();
	EXPECT(d.Start());
	d.loop.Advance(9900);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Checking);
	EXPECT_EQ(askedNext, 0);
	d.loop.Advance(200);
	EXPECT_EQ(askedNext, 1);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Pending);
	EXPECT(!d.store->Done());
	// The abandoned call answering late is ignored.
	slow.Answer(Referrer("strybok_click=" + kClick));
	d.loop.Advance(60000);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Pending);
	EXPECT_EQ(d.api.deferred.size(), Count(0));
	EXPECT_EQ(d.loop.TaskCount(), Count(0));
}

TEST(DeferredAndroid, GivesUpAfterThreeAttemptsAcrossLaunches)
{
	const std::shared_ptr<MemoryStore> store = std::make_shared<MemoryStore>();
	int asked = 0;
	for (int launch = 1; launch <= 3; ++launch)
	{
		Deferred d("android", store);
		d.config.installReferrerSources = {Answering(ForNow(), &asked)};
		EXPECT(d.Start());
		EXPECT_EQ(d.Status(), DeferredLinkStatus::Pending);
		EXPECT_EQ(store->Attempts(), std::to_string(launch));
		EXPECT(!store->Done());
	}
	EXPECT_EQ(asked, 3);

	Deferred fourth("android", store);
	fourth.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick), &asked)};
	EXPECT(fourth.Start());
	EXPECT_EQ(fourth.Status(), DeferredLinkStatus::Failed);
	EXPECT(store->Done());
	EXPECT_EQ(store->Attempts(), "3");
	EXPECT_EQ(asked, 3);
	EXPECT_EQ(fourth.api.deferred.size(), Count(0));

	Deferred fifth("android", store);
	fifth.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick), &asked)};
	EXPECT(fifth.Start());
	EXPECT_EQ(fifth.Status(), DeferredLinkStatus::NotFirstLaunch);
	EXPECT_EQ(asked, 3);

	// A stored value that is not a number we wrote counts as used up.
	const std::shared_ptr<MemoryStore> odd = std::make_shared<MemoryStore>();
	odd->values[kDeferredAttemptsKey] = "many";
	Deferred sixth("android", odd);
	sixth.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick), &asked)};
	EXPECT(sixth.Start());
	EXPECT_EQ(sixth.Status(), DeferredLinkStatus::Failed);
	EXPECT_EQ(asked, 3);
}

TEST(DeferredAndroid, ServerAnswersDecideTheStatus)
{
	struct Case
	{
		Reply reply;
		DeferredLinkStatus status;
		bool done;
		std::size_t requests;
	};
	const std::vector<Case> cases = {
		{Respond(404, "{\"error\":{\"code\":\"DEFERRED_LINK_NOT_FOUND\"}}"), DeferredLinkStatus::NotFound, true, 1},
		{Respond(400), DeferredLinkStatus::NotFound, true, 1},
		{Respond(422), DeferredLinkStatus::NotFound, true, 1},
		{Respond(410), DeferredLinkStatus::NotFound, true, 1},
		{Respond(429), DeferredLinkStatus::Pending, false, 1}, // not retried in this launch
		{Respond(500), DeferredLinkStatus::Pending, false, 3},
		{Respond(503), DeferredLinkStatus::Pending, false, 3},
		{NetworkError(), DeferredLinkStatus::Pending, false, 3},
		{Hang(), DeferredLinkStatus::Pending, false, 3},
		{Respond(204), DeferredLinkStatus::Failed, true, 1},
		{Respond(200, "not json"), DeferredLinkStatus::Failed, true, 1},
		{Respond(200, OkBody()), DeferredLinkStatus::Failed, true, 1},
		{Respond(200, DeferredBody("https://evil.example/invite777")), DeferredLinkStatus::Failed, true, 1},
		{Respond(200, "{\"linkId\":\"l\",\"appId\":\"a\",\"routePath\":\"//x\",\"url\":\"https://my-app.strybok.link/s\",\"deferred\":true}"),
			DeferredLinkStatus::Failed, true, 1},
	};
	for (const Case& testCase : cases)
	{
		Deferred d;
		d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick))};
		const Reply reply = testCase.reply;
		d.api.onDeferred = [reply](const HttpRequest&) { return reply; };
		d.Listen();
		EXPECT(d.Start());
		d.loop.Advance(5000);
		EXPECT_EQ(d.Status(), testCase.status);
		EXPECT_EQ(d.store->Done(), testCase.done);
		EXPECT_EQ(d.api.deferred.size(), testCase.requests);
		for (const HttpRequest& request : d.api.deferred)
		{
			EXPECT_EQ(request.body, d.api.deferred[0].body); // a retry sends the identical body
			EXPECT_EQ(request.url, kRedeemUrl);
		}
		EXPECT_EQ(d.received.size(), Count(0));
		EXPECT_EQ(d.errors.size(), Count(0));
		EXPECT_EQ(d.api.events.size(), Count(0));
		EXPECT_EQ(d.sdk->PendingCount(), Count(0));
		EXPECT_EQ(d.loop.TaskCount(), Count(0));
	}
}

TEST(DeferredAndroid, RetriesLikeResolvingAndThenDelivers)
{
	Deferred d;
	d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick))};
	d.config.retryDelayMs = 100;
	int calls = 0;
	d.api.onDeferred = [&calls](const HttpRequest&) {
		++calls;
		if (calls == 1)
		{
			return Respond(502);
		}
		if (calls == 2)
		{
			return NetworkError();
		}
		return Respond(200, DeferredBody());
	};
	d.Listen();
	bool ready = false;
	d.sdk->Start(nullptr, [&ready]() { ready = true; });
	d.loop.Advance(50);
	EXPECT_EQ(calls, 1);
	d.loop.Advance(100); // first pause: retryDelay
	EXPECT_EQ(calls, 2);
	d.loop.Advance(100);
	EXPECT_EQ(calls, 2);
	d.loop.Advance(100); // second pause: doubled
	EXPECT_EQ(calls, 3);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Delivered);
	EXPECT_EQ(d.received.size(), Count(1));
	EXPECT_EQ(d.api.deferred[0].body, d.api.deferred[2].body);
}

TEST(DeferredAndroid, ANormalLinkWinsBeforeTheRequest)
{
	Deferred d;
	int asked = 0;
	d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick), &asked)};
	d.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
	d.Listen();
	EXPECT(d.Start(kInvite));
	EXPECT_EQ(asked, 1);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Superseded);
	EXPECT(d.store->Done());
	EXPECT_EQ(d.api.deferred.size(), Count(0)); // the id is not even sent
	EXPECT_EQ(d.received.size(), Count(1));
	EXPECT(!d.received[0].isDeferred);

	// Also when the normal link could not be resolved: it was accepted.
	Deferred failing;
	failing.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick))};
	failing.api.onResolve = [](const HttpRequest&) { return Respond(404); };
	EXPECT(failing.Start(kInvite));
	EXPECT_EQ(failing.Status(), DeferredLinkStatus::Superseded);
	EXPECT_EQ(failing.api.deferred.size(), Count(0));
	EXPECT_EQ(failing.errors.size(), Count(1)); // the normal link's own error, nothing of the check

	// A URL that is not ours, and a manual Resolve, do not count.
	Deferred foreign;
	foreign.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick))};
	foreign.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
	foreign.Make();
	foreign.Listen();
	bool resolved = false;
	foreign.sdk->Resolve(kInvite, [&resolved](const ResolvedLink* link, const LinkError*) { resolved = link != nullptr; });
	EXPECT(foreign.Start("https://example.com/invite777"));
	EXPECT(resolved);
	EXPECT_EQ(foreign.Status(), DeferredLinkStatus::Delivered);
	EXPECT_EQ(foreign.received.size(), Count(1));
	EXPECT(foreign.received[0].isDeferred);
}

TEST(DeferredAndroid, ANormalLinkWinsWhileTheRequestIsInFlight)
{
	Deferred d;
	d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick))};
	d.config.timeoutMs = 5000;
	d.api.onDeferred = [](const HttpRequest&) { return Hang(); };
	d.Make();
	d.Listen();
	EXPECT(d.Start());
	EXPECT_EQ(d.api.deferred.size(), Count(1));
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Checking);

	d.sdk->HandleUrl(kInvite);
	d.loop.Settle();
	EXPECT_EQ(d.received.size(), Count(1));
	EXPECT(!d.received[0].isDeferred);

	EXPECT(d.api.Complete(d.api.deferredIds[0], Respond(200, DeferredBody())));
	d.loop.Advance(60000);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Superseded);
	EXPECT(d.store->Done());
	EXPECT_EQ(d.received.size(), Count(1)); // one launch, one navigation
	EXPECT_EQ(d.sdk->PendingCount(), Count(0));
}

TEST(DeferredAndroid, StopDuringTheCheckDeliversNothing)
{
	// While a source is being asked.
	Deferred asking;
	HeldSource held;
	asking.config.installReferrerSources = {held.Source()};
	asking.Make();
	asking.Listen();
	EXPECT(asking.Start());
	asking.sdk->Stop();
	held.Answer(Referrer("strybok_click=" + kClick));
	asking.loop.Advance(60000);
	EXPECT_EQ(asking.api.deferred.size(), Count(0));
	EXPECT_EQ(asking.loop.TaskCount(), Count(0));
	EXPECT(!asking.store->Done());

	// While the request is in flight.
	Deferred d;
	d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick))};
	d.api.onDeferred = [](const HttpRequest&) { return Hang(); };
	d.Make();
	d.Listen();
	EXPECT(d.Start());
	EXPECT_EQ(d.api.deferred.size(), Count(1));
	d.sdk->Stop();
	EXPECT_EQ(d.api.cancelled.size(), Count(1));
	EXPECT_EQ(d.loop.TaskCount(), Count(0));
	EXPECT(d.api.Complete(d.api.deferredIds[0], Respond(200, DeferredBody())));
	d.loop.Advance(60000);
	EXPECT_EQ(d.received.size(), Count(0));
	EXPECT_EQ(d.statuses.back(), DeferredLinkStatus::Checking);
	EXPECT(!d.store->Done());
	EXPECT_EQ(d.store->Attempts(), "1"); // the attempt is used up
	d.sdk->EnableDeferredLinks(); // harmless
	d.Paste(kPasted);
	EXPECT_EQ(d.pasted.back(), PastedLinkResult::Disabled);

	// Stopped before the check's turn came.
	Deferred early;
	int asked = 0;
	early.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick), &asked)};
	early.Make();
	early.sdk->Start(nullptr);
	early.sdk->Stop();
	early.loop.Advance(60000);
	EXPECT_EQ(asked, 0);
	EXPECT_EQ(early.store->writes.size(), Count(0));
}

TEST(DeferredAndroid, CanBeSwitchedOnLaterAndRunsOnce)
{
	Deferred d;
	d.config.deferredLinks = false;
	d.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
	d.Make();
	d.Listen();
	EXPECT(d.Start());
	d.loop.Advance(60000);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Disabled);
	EXPECT_EQ(d.store->reads, 0);

	// Sources handed over before switching on (for example after a consent screen).
	int asked = 0;
	d.sdk->SetInstallReferrerSources({Answering(Referrer("strybok_click=" + kClick), &asked)});
	d.sdk->EnableDeferredLinks();
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Pending);
	EXPECT_EQ(asked, 0); // not inside the call
	d.sdk->EnableDeferredLinks();
	d.loop.Settle();
	EXPECT_EQ(asked, 1);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Delivered);
	EXPECT_EQ(d.received.size(), Count(1));
	d.sdk->EnableDeferredLinks();
	d.sdk->SetInstallReferrerSources({Answering(Referrer("strybok_click=" + kOtherClick), &asked)});
	d.loop.Advance(60000);
	EXPECT_EQ(asked, 1);
	EXPECT_EQ(d.api.deferred.size(), Count(1));
	EXPECT_EQ(d.statuses.size(), Count(3));
	EXPECT_EQ(d.statuses[0], DeferredLinkStatus::Pending);

	// Switched on before Start: runs after Start, as usual.
	Deferred before;
	before.config.deferredLinks = false;
	before.config.installReferrerSources = {Answering(Referrer("x=y"), &asked)};
	before.Make();
	before.sdk->EnableDeferredLinks();
	before.loop.Advance(60000);
	EXPECT_EQ(asked, 1);
	EXPECT_EQ(before.Status(), DeferredLinkStatus::Pending);
	EXPECT(before.Start());
	EXPECT_EQ(asked, 2);
	EXPECT_EQ(before.Status(), DeferredLinkStatus::NoReferrer);
}

TEST(DeferredAndroid, ARestartAfterEachStepNeverDeliversTwice)
{
	const std::shared_ptr<MemoryStore> store = std::make_shared<MemoryStore>();

	// 1. Killed while the source was being asked: one attempt is gone, nothing else.
	{
		Deferred d("android", store);
		HeldSource held;
		d.config.installReferrerSources = {held.Source()};
		EXPECT(d.Start());
		EXPECT_EQ(store->Attempts(), "1");
	}
	// 2. Killed while the request was in flight (the server may have used the id up).
	{
		Deferred d("android", store);
		d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick))};
		d.api.onDeferred = [](const HttpRequest&) { return Hang(); };
		d.config.timeoutMs = 5000;
		EXPECT(d.Start());
		EXPECT_EQ(d.api.deferred.size(), Count(1));
		EXPECT_EQ(store->Attempts(), "2");
		EXPECT(!store->Done());
	}
	// 3. The next launch sends the same id again and delivers.
	{
		Deferred d("android", store);
		d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick))};
		d.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
		d.Make();
		d.Listen();
		EXPECT(d.Start());
		EXPECT_EQ(store->Attempts(), "3");
		EXPECT(d.api.deferred[0].body.find(kClick) != std::string::npos);
		EXPECT_EQ(d.received.size(), Count(1));
		EXPECT(store->Done());
	}
	// 4. Every later launch does nothing: no source, no request, no delivery.
	for (int launch = 0; launch < 2; ++launch)
	{
		Deferred d("android", store);
		int asked = 0;
		d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick), &asked)};
		d.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
		d.Make();
		d.Listen();
		EXPECT(d.Start());
		d.loop.Advance(60000);
		EXPECT_EQ(d.Status(), DeferredLinkStatus::NotFirstLaunch);
		EXPECT_EQ(asked, 0);
		EXPECT_EQ(d.api.deferred.size(), Count(0));
		EXPECT_EQ(d.received.size(), Count(0));
	}
	// Nothing but the two values was ever stored.
	EXPECT_EQ(store->values.size(), Count(2));
	for (const std::string& write : store->writes)
	{
		EXPECT(write.find("dc_") == std::string::npos);
	}
}

TEST(DeferredAndroid, StoreFailuresNeverRiskASecondDelivery)
{
	int asked = 0;
	// Reading fails: nothing is asked or sent.
	Deferred unreadable;
	unreadable.store->failReads = true;
	unreadable.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick), &asked)};
	EXPECT(unreadable.Start());
	EXPECT_EQ(unreadable.Status(), DeferredLinkStatus::Pending);
	EXPECT_EQ(asked, 0);
	EXPECT_EQ(unreadable.api.deferred.size(), Count(0));

	// The attempt cannot be counted: the source is not asked.
	Deferred uncounted;
	uncounted.store->failWritesTo = kDeferredAttemptsKey;
	uncounted.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick), &asked)};
	EXPECT(uncounted.Start());
	EXPECT_EQ(uncounted.Status(), DeferredLinkStatus::Pending);
	EXPECT_EQ(asked, 0);
	EXPECT_EQ(uncounted.api.deferred.size(), Count(0));

	// `done` cannot be stored before delivery: the link is not delivered.
	Deferred unfinished;
	unfinished.store->failWritesTo = kDeferredDoneKey;
	unfinished.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick), &asked)};
	unfinished.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
	unfinished.Make();
	unfinished.Listen();
	EXPECT(unfinished.Start());
	EXPECT_EQ(asked, 1);
	EXPECT_EQ(unfinished.api.deferred.size(), Count(1));
	EXPECT_EQ(unfinished.Status(), DeferredLinkStatus::Failed);
	EXPECT_EQ(unfinished.received.size(), Count(0));
	EXPECT_EQ(unfinished.sdk->PendingCount(), Count(0));

	// Elsewhere a failed `done` write is ignored: the status is what was found.
	Deferred organic;
	organic.store->failWritesTo = kDeferredDoneKey;
	organic.config.installReferrerSources = {Answering(Referrer("utm_source=google-play"))};
	EXPECT(organic.Start());
	EXPECT_EQ(organic.Status(), DeferredLinkStatus::NoReferrer);
}

TEST(DeferredAndroid, WorksSynchronouslyAndSurvivesBeingDestroyedInAHandler)
{
	Deferred d;
	d.api.synchronous = true;
	d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick))};
	d.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
	d.Make();
	int delivered = 0;
	d.sdk->SetLinkHandler([&d, &delivered](const ResolvedLink& link) {
		delivered += link.isDeferred ? 1 : 0;
		d.sdk.reset();
	});
	d.sdk->SetListening(true);
	d.sdk->Start(nullptr);
	d.loop.Advance(60000);
	EXPECT_EQ(delivered, 1);
	EXPECT(d.sdk == nullptr);
	EXPECT_EQ(d.loop.TaskCount(), Count(0));

	// Destroyed from the status handler: nothing follows.
	Deferred s;
	s.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick))};
	s.Make();
	s.Listen();
	s.sdk->SetDeferredLinkStatusHandler([&s](DeferredLinkStatus) { s.sdk.reset(); });
	s.sdk->Start(nullptr);
	s.loop.Advance(60000);
	EXPECT(s.sdk == nullptr);
	EXPECT_EQ(s.api.deferred.size(), Count(0));
	EXPECT_EQ(s.store->writes.size(), Count(0));
}

// --- iOS: pasted link ---

TEST(DeferredPaste, WithoutMatchingNothingIsAutomaticAndPastingStaysPossible)
{
	Deferred d("ios");
	int asked = 0;
	d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick), &asked)};
	d.Make();
	d.Listen();
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Pending);
	EXPECT(d.Start());
	d.loop.Advance(60000);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Unavailable);
	EXPECT_EQ(asked, 0); // no install referrer on iOS
	EXPECT_EQ(d.api.deferred.size(), Count(0));
	EXPECT_EQ(d.store->writes.size(), Count(0)); // `done` is not stored
	d.sdk->SkipPastedLink(); // nothing to skip
	d.loop.Advance(60000);
	EXPECT_EQ(d.api.deferred.size(), Count(0));
}

TEST(DeferredPaste, RedeemsThePastedLinkAndDeliversIt)
{
	Deferred d("ios");
	d.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
	d.Make();
	bool doneWhenDelivered = false;
	d.sdk->SetLinkHandler([&d, &doneWhenDelivered](const ResolvedLink& link) {
		doneWhenDelivered = d.store->Done();
		d.received.push_back(link);
	});
	d.sdk->SetListening(true);
	EXPECT(d.Start());

	d.Paste("  " + kPasted + "&utm_source=secret#frag\n");
	EXPECT_EQ(d.pasted.size(), Count(0));
	d.loop.Settle();
	EXPECT_EQ(d.pasted.size(), Count(1));
	EXPECT_EQ(d.pasted[0], PastedLinkResult::Delivered);

	EXPECT_EQ(d.api.deferred.size(), Count(1));
	const HttpRequest& request = d.api.deferred[0];
	EXPECT_EQ(request.method, "POST");
	EXPECT_EQ(request.url, kRedeemUrl);
	EXPECT_EQ(HeaderOf(request, "Content-Type"), "application/json");
	EXPECT_EQ(HeaderOf(request, "Accept"), "application/json");
	// Only the click id, never the pasted URL.
	EXPECT_EQ(request.body,
		"{\"platform\":\"ios\",\"clickId\":\"" + kClick + "\",\"bundleId\":\"com.example.Game\",\"sdkVersion\":\"" +
			std::string(kSdkVersion) + "\"}");

	EXPECT_EQ(d.received.size(), Count(1));
	EXPECT_EQ(d.received[0].uri, "https://my-app.strybok.link/invite777"); // the answer's url, without the click id
	EXPECT(d.received[0].isDeferred);
	EXPECT(!d.received[0].isProbabilisticMatch);
	EXPECT(doneWhenDelivered);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Delivered);
	EXPECT_EQ(d.store->values.size(), Count(1)); // `done` only: flow A uses no attempts
	d.loop.Advance(60000);
	EXPECT_EQ(d.api.events.size(), Count(0));
	EXPECT_EQ(d.errors.size(), Count(0));

	ResolvedLink initial;
	EXPECT(!d.sdk->TakeInitialLink(initial));

	// Once handled, always handled - in this launch and in the next.
	d.Paste(kPasted);
	EXPECT_EQ(d.pasted.back(), PastedLinkResult::AlreadyHandled);
	Deferred next("ios", d.store);
	EXPECT(next.Start());
	EXPECT_EQ(next.Status(), DeferredLinkStatus::NotFirstLaunch);
	next.Paste(kPasted);
	EXPECT_EQ(next.pasted.back(), PastedLinkResult::AlreadyHandled);
	EXPECT_EQ(next.api.deferred.size(), Count(0));
	EXPECT_EQ(d.api.deferred.size(), Count(1));
}

TEST(DeferredPaste, WorksBeforeStartAndQueuesUntilAListenerAttaches)
{
	Deferred d("ios");
	d.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
	d.Make();
	d.Paste(kPasted);
	d.loop.Settle();
	EXPECT_EQ(d.pasted.back(), PastedLinkResult::Delivered);
	EXPECT_EQ(d.sdk->PendingCount(), Count(1));
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Delivered);
	// The check finds the work done and keeps the status.
	EXPECT(d.Start());
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Delivered);
	d.Listen();
	EXPECT_EQ(d.received.size(), Count(1));
}

TEST(DeferredPaste, AnythingElseIsNotOursAndIsSentNowhere)
{
	Deferred d("ios");
	d.Make();
	d.Listen();
	EXPECT(d.Start());
	const std::vector<std::string> texts = {
		"",
		"   ",
		"my password is hunter2",
		"https://my-app.strybok.link/invite777",
		"https://my-app.strybok.link/invite777?strybok_click=dc_wrong",
		"https://other.example/invite777?strybok_click=" + kClick,
		"http://my-app.strybok.link/invite777?strybok_click=" + kClick,
		"https://my-app.strybok.link/invite777/more?strybok_click=" + kClick,
		"strybok_click=" + kClick,
	};
	for (const std::string& text : texts)
	{
		d.Paste(text);
		EXPECT_EQ(d.pasted.back(), PastedLinkResult::NotOurs);
	}
	d.loop.Advance(60000);
	EXPECT_EQ(d.pasted.size(), texts.size());
	EXPECT_EQ(d.api.deferred.size(), Count(0));
	EXPECT_EQ(d.store->writes.size(), Count(0));
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Unavailable);
	EXPECT_EQ(d.errors.size(), Count(0));
}

TEST(DeferredPaste, IsDeliveredEvenAfterANormalLink)
{
	Deferred d("ios");
	d.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
	d.Make();
	d.Listen();
	EXPECT(d.Start(kInvite));
	EXPECT_EQ(d.received.size(), Count(1));
	d.Paste(kPasted);
	d.loop.Settle();
	EXPECT_EQ(d.pasted.back(), PastedLinkResult::Delivered);
	EXPECT_EQ(d.received.size(), Count(2));
	EXPECT(d.received[1].isDeferred);
	// The pasted link does not disturb duplicate detection of normal links, and is not subject to it.
	d.loop.Advance(60000);
	EXPECT_EQ(d.api.events.size(), Count(1)); // the normal link's only
}

TEST(DeferredPaste, ServerAnswersDecideTheResult)
{
	struct Case
	{
		Reply reply;
		PastedLinkResult result;
		bool done;
		std::size_t requests;
		DeferredLinkStatus status;
	};
	const std::vector<Case> cases = {
		{Respond(404), PastedLinkResult::NotFound, true, 1, DeferredLinkStatus::NotFound},
		{Respond(422), PastedLinkResult::NotFound, true, 1, DeferredLinkStatus::NotFound},
		{Respond(200, "{}"), PastedLinkResult::Failed, true, 1, DeferredLinkStatus::Failed},
		{Respond(200, DeferredBody("https://evil.example/invite777")), PastedLinkResult::Failed, true, 1, DeferredLinkStatus::Failed},
		{Respond(429), PastedLinkResult::Failed, false, 1, DeferredLinkStatus::Unavailable},
		{Respond(500), PastedLinkResult::Failed, false, 3, DeferredLinkStatus::Unavailable},
		{NetworkError(), PastedLinkResult::Failed, false, 3, DeferredLinkStatus::Unavailable},
		{Hang(), PastedLinkResult::Failed, false, 3, DeferredLinkStatus::Unavailable},
	};
	for (const Case& testCase : cases)
	{
		Deferred d("ios");
		const Reply reply = testCase.reply;
		d.api.onDeferred = [reply](const HttpRequest&) { return reply; };
		d.Make();
		d.Listen();
		EXPECT(d.Start());
		d.Paste(kPasted);
		d.loop.Advance(5000);
		EXPECT_EQ(d.pasted.size(), Count(1));
		EXPECT_EQ(d.pasted[0], testCase.result);
		EXPECT_EQ(d.store->Done(), testCase.done);
		EXPECT_EQ(d.api.deferred.size(), testCase.requests);
		EXPECT_EQ(d.Status(), testCase.status);
		EXPECT_EQ(d.received.size(), Count(0));
		EXPECT_EQ(d.errors.size(), Count(0));
		EXPECT_EQ(d.store->Attempts(), "<none>");

		// After a temporary failure the person can paste again; after a definite answer not.
		d.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
		d.Paste(kPasted);
		d.loop.Advance(5000);
		EXPECT_EQ(d.pasted.back(), testCase.done ? PastedLinkResult::AlreadyHandled : PastedLinkResult::Delivered);
		EXPECT_EQ(d.received.size(), testCase.done ? Count(0) : Count(1));
	}
}

TEST(DeferredPaste, CallsAreHandledOneAfterAnother)
{
	Deferred d("ios");
	d.config.timeoutMs = 5000;
	d.api.onDeferred = [](const HttpRequest&) { return Hang(); };
	d.Make();
	d.Listen();
	EXPECT(d.Start());
	d.Paste(kPasted);
	d.Paste(kPasted);
	d.Paste("not a link");
	d.loop.Settle();
	EXPECT_EQ(d.api.deferred.size(), Count(1)); // the second call waits for the first
	EXPECT_EQ(d.pasted.size(), Count(0));
	EXPECT(d.api.Complete(d.api.deferredIds[0], Respond(200, DeferredBody())));
	EXPECT_EQ(d.pasted.size(), Count(3));
	EXPECT_EQ(d.pasted[0], PastedLinkResult::Delivered);
	EXPECT_EQ(d.pasted[1], PastedLinkResult::AlreadyHandled);
	EXPECT_EQ(d.pasted[2], PastedLinkResult::AlreadyHandled);
	EXPECT_EQ(d.api.deferred.size(), Count(1));
	EXPECT_EQ(d.received.size(), Count(1));
}

TEST(DeferredPaste, IsDisabledOffIosAndDropsItsCallbackWhenStopped)
{
	Deferred android;
	android.config.installReferrerSources = {Answering(ForNow())};
	EXPECT(android.Start());
	android.Paste(kPasted);
	EXPECT_EQ(android.pasted.back(), PastedLinkResult::Disabled);
	EXPECT_EQ(android.api.deferred.size(), Count(0));

	Deferred d("ios");
	d.config.timeoutMs = 5000;
	d.api.onDeferred = [](const HttpRequest&) { return Hang(); };
	d.Make();
	d.Listen();
	EXPECT(d.Start());
	d.Paste(kPasted);
	d.Paste(kPasted);
	d.loop.Settle();
	d.sdk->Stop();
	EXPECT(d.api.Complete(d.api.deferredIds[0], Respond(200, DeferredBody())));
	d.loop.Advance(60000);
	EXPECT_EQ(d.pasted.size(), Count(0));
	EXPECT_EQ(d.received.size(), Count(0));
	EXPECT(!d.store->Done());
	EXPECT_EQ(d.loop.TaskCount(), Count(0));

	// A store that cannot be read: nothing is sent.
	Deferred unreadable("ios");
	unreadable.Make();
	unreadable.store->failReads = true;
	unreadable.Paste(kPasted);
	EXPECT_EQ(unreadable.pasted.back(), PastedLinkResult::Failed);
	EXPECT_EQ(unreadable.api.deferred.size(), Count(0));

	// `done` cannot be stored: the link is not delivered.
	Deferred unwritable("ios");
	unwritable.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
	unwritable.Make();
	unwritable.Listen();
	unwritable.store->failWritesTo = kDeferredDoneKey;
	unwritable.Paste(kPasted);
	unwritable.loop.Settle();
	EXPECT_EQ(unwritable.pasted.back(), PastedLinkResult::Failed);
	EXPECT_EQ(unwritable.Status(), DeferredLinkStatus::Failed);
	EXPECT_EQ(unwritable.received.size(), Count(0));
}

// --- iOS: probabilistic matching ---

namespace
{

Deferred& Matching(Deferred& d)
{
	d.config.allowProbabilisticMatching = true;
	d.api.onDeferred = [](const HttpRequest& request) {
		return request.url == kMatchUrl ? Respond(200, MatchedBody()) : Respond(200, DeferredBody());
	};
	return d;
}

} // namespace

TEST(DeferredMatching, AsksTheServerOnceAndMarksTheLinkAsAGuess)
{
	Deferred d("ios");
	Matching(d);
	d.config.locale = []() { return std::string("uk-UA"); };
	d.config.deviceFamily = []() { return std::string("ipad"); };
	d.Make();
	bool doneWhenDelivered = false;
	d.sdk->SetLinkHandler([&d, &doneWhenDelivered](const ResolvedLink& link) {
		doneWhenDelivered = d.store->Done();
		d.received.push_back(link);
	});
	d.sdk->SetListening(true);
	bool ready = false;
	d.sdk->Start(nullptr, [&ready]() { ready = true; });
	EXPECT(ready);
	EXPECT_EQ(d.api.deferred.size(), Count(0)); // not on the start path
	d.loop.Settle();

	EXPECT_EQ(d.api.deferred.size(), Count(1));
	const HttpRequest& request = d.api.deferred[0];
	EXPECT_EQ(request.method, "POST");
	EXPECT_EQ(request.url, kMatchUrl);
	EXPECT_EQ(HeaderOf(request, "Content-Type"), "application/json");
	EXPECT_EQ(HeaderOf(request, "Accept"), "application/json");
	EXPECT_EQ(request.body,
		"{\"platform\":\"ios\",\"bundleId\":\"com.example.Game\",\"deviceFamily\":\"ipad\",\"locale\":\"uk-UA\",\"sdkVersion\":\"" +
			std::string(kSdkVersion) + "\"}");

	EXPECT_EQ(d.received.size(), Count(1));
	EXPECT(d.received[0].isDeferred);
	EXPECT(d.received[0].isProbabilisticMatch);
	EXPECT_EQ(d.received[0].uri, "https://my-app.strybok.link/invite777");
	EXPECT(doneWhenDelivered);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Delivered);
	EXPECT_EQ(d.statuses.size(), Count(2));
	EXPECT_EQ(d.statuses[0], DeferredLinkStatus::Checking);
	EXPECT_EQ(d.store->writes[0], "strybok.deferred.attempts=1");
	EXPECT_EQ(d.store->writes[1], "strybok.deferred.done=1");
	d.loop.Advance(60000);
	EXPECT_EQ(d.api.events.size(), Count(0));
	EXPECT_EQ(d.errors.size(), Count(0));
	EXPECT_EQ(d.api.deferred.size(), Count(1));

	Deferred next("ios", d.store);
	Matching(next);
	EXPECT(next.Start());
	EXPECT_EQ(next.Status(), DeferredLinkStatus::NotFirstLaunch);
	EXPECT_EQ(next.api.deferred.size(), Count(0));
}

TEST(DeferredMatching, ServerAnswersDecideTheStatus)
{
	struct Case
	{
		Reply reply;
		DeferredLinkStatus status;
		bool done;
		std::size_t requests;
	};
	const std::vector<Case> cases = {
		{Respond(404), DeferredLinkStatus::NotFound, true, 1}, // nothing fitted, or more than one did
		{Respond(422), DeferredLinkStatus::NotFound, true, 1},
		{Respond(200, DeferredBody()), DeferredLinkStatus::Failed, true, 1}, // `matched` is missing
		{Respond(200, DeferredBody("https://my-app.strybok.link/invite777", ",\"matched\":true")), DeferredLinkStatus::Failed, true, 1},
		{Respond(200, "<html>"), DeferredLinkStatus::Failed, true, 1},
		{Respond(429), DeferredLinkStatus::Pending, false, 1},
		{Respond(500), DeferredLinkStatus::Pending, false, 3},
		{NetworkError(), DeferredLinkStatus::Pending, false, 3},
		{Hang(), DeferredLinkStatus::Pending, false, 3},
	};
	for (const Case& testCase : cases)
	{
		Deferred d("ios");
		d.config.allowProbabilisticMatching = true;
		const Reply reply = testCase.reply;
		d.api.onDeferred = [reply](const HttpRequest&) { return reply; };
		d.Make();
		d.Listen();
		EXPECT(d.Start());
		d.loop.Advance(5000);
		EXPECT_EQ(d.Status(), testCase.status);
		EXPECT_EQ(d.store->Done(), testCase.done);
		EXPECT_EQ(d.store->Attempts(), "1");
		EXPECT_EQ(d.api.deferred.size(), testCase.requests);
		for (const HttpRequest& request : d.api.deferred)
		{
			EXPECT_EQ(request.url, kMatchUrl);
			EXPECT_EQ(request.body, d.api.deferred[0].body);
		}
		EXPECT_EQ(d.received.size(), Count(0));
		EXPECT_EQ(d.errors.size(), Count(0));
	}
}

TEST(DeferredMatching, GivesUpAfterThreeAttemptsAcrossLaunches)
{
	const std::shared_ptr<MemoryStore> store = std::make_shared<MemoryStore>();
	for (int launch = 1; launch <= 3; ++launch)
	{
		Deferred d("ios", store);
		d.config.allowProbabilisticMatching = true;
		d.api.onDeferred = [](const HttpRequest&) { return Respond(503); };
		EXPECT(d.Start());
		d.loop.Advance(5000);
		EXPECT_EQ(d.Status(), DeferredLinkStatus::Pending);
		EXPECT_EQ(store->Attempts(), std::to_string(launch));
	}
	Deferred fourth("ios", store);
	Matching(fourth);
	EXPECT(fourth.Start());
	EXPECT_EQ(fourth.Status(), DeferredLinkStatus::Failed);
	EXPECT(store->Done());
	EXPECT_EQ(fourth.api.deferred.size(), Count(0));
}

TEST(DeferredMatching, NeedsADeviceFamilyAndALocaleFromTheApp)
{
	const auto expectUnavailable = [](Deferred& d) {
		d.Make();
		d.Listen();
		EXPECT(d.Start());
		d.loop.Advance(60000);
		EXPECT_EQ(d.Status(), DeferredLinkStatus::Unavailable);
		EXPECT(d.store->Done());
		EXPECT_EQ(d.store->Attempts(), "1");
		EXPECT_EQ(d.api.deferred.size(), Count(0));
	};

	Deferred noFamily("ios");
	Matching(noFamily).config.deviceFamily = nullptr;
	expectUnavailable(noFamily);

	Deferred mac("ios");
	Matching(mac).config.deviceFamily = []() { return std::string("mac"); };
	expectUnavailable(mac);

	Deferred noLocale("ios");
	Matching(noLocale).config.locale = nullptr;
	expectUnavailable(noLocale);

	Deferred emptyLocale("ios");
	Matching(emptyLocale).config.locale = []() { return std::string(); };
	expectUnavailable(emptyLocale);

	Deferred oddLocale("ios");
	Matching(oddLocale).config.locale = []() { return std::string("English (United Kingdom)"); };
	expectUnavailable(oddLocale);
}

TEST(DeferredMatching, ANormalLinkWins)
{
	// Before the request: nothing is sent.
	Deferred before("ios");
	Matching(before).Make();
	before.Listen();
	EXPECT(before.Start(kInvite));
	EXPECT_EQ(before.Status(), DeferredLinkStatus::Superseded);
	EXPECT(before.store->Done());
	EXPECT_EQ(before.api.deferred.size(), Count(0));
	EXPECT_EQ(before.received.size(), Count(1));

	// While the request is in flight: the answer is dropped.
	Deferred during("ios");
	during.config.allowProbabilisticMatching = true;
	during.config.timeoutMs = 5000;
	during.api.onDeferred = [](const HttpRequest&) { return Hang(); };
	during.Make();
	during.Listen();
	EXPECT(during.Start());
	EXPECT_EQ(during.api.deferred.size(), Count(1));
	during.sdk->HandleUrl(kInvite);
	during.loop.Settle();
	EXPECT(during.api.Complete(during.api.deferredIds[0], Respond(200, MatchedBody())));
	EXPECT_EQ(during.Status(), DeferredLinkStatus::Superseded);
	EXPECT_EQ(during.received.size(), Count(1));
	EXPECT(!during.received[0].isDeferred);
}

TEST(DeferredMatching, WaitsForThePasteWhenThePasteboardMayHoldALink)
{
	const auto probe = [](bool answer, int* calls) {
		return [answer, calls](std::function<void(bool)> done) {
			++*calls;
			done(answer);
		};
	};

	// Not ours: matching starts.
	int probed = 0;
	Deferred notOurs("ios");
	Matching(notOurs).config.pasteboardMayContainLink = probe(true, &probed);
	notOurs.Make();
	notOurs.Listen();
	EXPECT(notOurs.Start());
	notOurs.loop.Advance(60000);
	EXPECT_EQ(probed, 1);
	EXPECT_EQ(notOurs.Status(), DeferredLinkStatus::AwaitingPaste);
	EXPECT_EQ(notOurs.api.deferred.size(), Count(0));
	EXPECT_EQ(notOurs.store->writes.size(), Count(0));
	notOurs.Paste("https://example.com/something");
	EXPECT_EQ(notOurs.pasted.back(), PastedLinkResult::NotOurs);
	notOurs.loop.Settle();
	EXPECT_EQ(notOurs.api.deferred.size(), Count(1));
	EXPECT_EQ(notOurs.api.deferred[0].url, kMatchUrl);
	EXPECT_EQ(notOurs.Status(), DeferredLinkStatus::Delivered);
	EXPECT(notOurs.received[0].isProbabilisticMatch);
	// Matching ran once; another foreign paste does not start it again.
	notOurs.Paste("still not a link");
	notOurs.sdk->SkipPastedLink();
	notOurs.loop.Advance(60000);
	EXPECT_EQ(notOurs.api.deferred.size(), Count(1));
	EXPECT_EQ(notOurs.pasted.back(), PastedLinkResult::AlreadyHandled);

	// The person moved on without pasting.
	Deferred skipped("ios");
	Matching(skipped).config.pasteboardMayContainLink = probe(true, &probed);
	skipped.Make();
	skipped.Listen();
	EXPECT(skipped.Start());
	EXPECT_EQ(skipped.Status(), DeferredLinkStatus::AwaitingPaste);
	skipped.sdk->SkipPastedLink();
	skipped.sdk->SkipPastedLink();
	skipped.loop.Settle();
	EXPECT_EQ(skipped.api.deferred.size(), Count(1));
	EXPECT_EQ(skipped.api.deferred[0].url, kMatchUrl);
	EXPECT_EQ(skipped.Status(), DeferredLinkStatus::Delivered);
	EXPECT_EQ(skipped.received.size(), Count(1));

	// The pasted link is ours: delivered exactly, and matching never runs.
	Deferred ours("ios");
	Matching(ours).config.pasteboardMayContainLink = probe(true, &probed);
	ours.Make();
	ours.Listen();
	EXPECT(ours.Start());
	ours.Paste(kPasted);
	ours.loop.Settle();
	EXPECT_EQ(ours.pasted.back(), PastedLinkResult::Delivered);
	ours.sdk->SkipPastedLink();
	ours.loop.Advance(60000);
	EXPECT_EQ(ours.api.deferred.size(), Count(1));
	EXPECT_EQ(ours.api.deferred[0].url, kRedeemUrl);
	EXPECT_EQ(ours.received.size(), Count(1));
	EXPECT(!ours.received[0].isProbabilisticMatch);
	EXPECT_EQ(ours.store->Attempts(), "<none>");

	// A temporary failure of the paste keeps waiting; the person can paste again.
	Deferred retry("ios");
	Matching(retry).config.pasteboardMayContainLink = probe(true, &probed);
	retry.api.onDeferred = [](const HttpRequest&) { return Respond(429); };
	retry.Make();
	retry.Listen();
	EXPECT(retry.Start());
	retry.Paste(kPasted);
	retry.loop.Settle();
	EXPECT_EQ(retry.pasted.back(), PastedLinkResult::Failed);
	EXPECT_EQ(retry.Status(), DeferredLinkStatus::AwaitingPaste);
	EXPECT_EQ(retry.api.deferred.size(), Count(1));

	// Nothing to paste: matching starts at once.
	Deferred empty("ios");
	Matching(empty).config.pasteboardMayContainLink = probe(false, &probed);
	empty.Make();
	empty.Listen();
	EXPECT(empty.Start());
	EXPECT_EQ(empty.api.deferred.size(), Count(1));
	EXPECT_EQ(empty.api.deferred[0].url, kMatchUrl);

	// A probe that never answers counts as "no" after ten seconds.
	Deferred silent("ios");
	Matching(silent).config.pasteboardMayContainLink = [](std::function<void(bool)>) {};
	silent.Make();
	silent.Listen();
	EXPECT(silent.Start());
	silent.loop.Advance(9000);
	EXPECT_EQ(silent.api.deferred.size(), Count(0));
	EXPECT_EQ(silent.Status(), DeferredLinkStatus::Checking);
	silent.loop.Advance(2000);
	EXPECT_EQ(silent.api.deferred.size(), Count(1));
	EXPECT_EQ(silent.Status(), DeferredLinkStatus::Delivered);
}

TEST(DeferredMatching, OnlyOneOfTheTwoFlowsDelivers)
{
	// A paste while the match request is in flight: the paste wins, the match answer is dropped.
	Deferred d("ios");
	d.config.allowProbabilisticMatching = true;
	d.config.timeoutMs = 5000;
	d.api.onDeferred = [](const HttpRequest& request) {
		return request.url == kMatchUrl ? Hang() : Respond(200, DeferredBody());
	};
	d.Make();
	d.Listen();
	EXPECT(d.Start());
	EXPECT_EQ(d.api.deferred.size(), Count(1));
	d.Paste(kPasted);
	d.loop.Settle();
	EXPECT_EQ(d.pasted.back(), PastedLinkResult::Delivered);
	EXPECT(d.api.Complete(d.api.deferredIds[0], Respond(200, MatchedBody())));
	d.loop.Advance(60000);
	EXPECT_EQ(d.received.size(), Count(1));
	EXPECT(!d.received[0].isProbabilisticMatch);
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Delivered);

	// The other way round: the match delivers while the paste request is in flight.
	Deferred m("ios");
	m.config.allowProbabilisticMatching = true;
	m.config.timeoutMs = 5000;
	m.api.onDeferred = [](const HttpRequest& request) {
		return request.url == kMatchUrl ? Respond(200, MatchedBody()) : Hang();
	};
	m.Make();
	m.Listen();
	m.Paste(kPasted); // before start: the paste request hangs
	EXPECT(m.Start());
	EXPECT_EQ(m.received.size(), Count(1));
	EXPECT(m.received[0].isProbabilisticMatch);
	EXPECT(m.api.Complete(m.api.deferredIds[0], Respond(200, DeferredBody())));
	EXPECT_EQ(m.pasted.back(), PastedLinkResult::AlreadyHandled);
	EXPECT_EQ(m.received.size(), Count(1));
}

// ---------------------------------------------------------------------------------------------
// Links without an API call (SDK contract; decision 0035): the shared test vectors in
// Tests/Fixtures, read with the core's own JSON reader, and the behaviour around them.
// ---------------------------------------------------------------------------------------------

namespace
{

/** The copy of the shared vectors next to this file (the runner compiles it by its absolute path). */
std::string VectorsPath()
{
	const std::string here = __FILE__;
	const std::size_t slash = here.find_last_of("/\\");
	const std::string folder = slash == std::string::npos ? std::string(".") : here.substr(0, slash);
	return folder + "/Fixtures/parameter-links-vectors.json";
}

/** The parsed vectors file; `Null` when it could not be read. */
const json::Value& VectorsFile()
{
	static json::Value root;
	static bool loaded = false;
	if (!loaded)
	{
		loaded = true;
		std::string text;
		if (std::FILE* file = std::fopen(VectorsPath().c_str(), "rb"))
		{
			char buffer[4096];
			std::size_t count = 0;
			while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
			{
				text.append(buffer, count);
			}
			std::fclose(file);
		}
		if (!json::Parse(text, root) || root.type != json::Type::Object)
		{
			root = json::Value();
		}
	}
	return root;
}

std::vector<std::string> StringItems(const json::Value* list)
{
	std::vector<std::string> items;
	if (list != nullptr && list->type == json::Type::Array)
	{
		for (const json::Value& item : list->items)
		{
			items.push_back(item.text);
		}
	}
	return items;
}

const json::Value* VectorList(const char* name)
{
	const json::Value* list = VectorsFile().Find(name);
	return list != nullptr && list->type == json::Type::Array ? list : nullptr;
}

/** The `parameterLinks` of a case; unset for `null`. */
std::optional<ParameterLinks> OptionOf(const json::Value& entry)
{
	const json::Value* option = entry.Find("parameterLinks");
	if (option == nullptr || option->type != json::Type::Object)
	{
		return std::nullopt;
	}
	ParameterLinks parameterLinks;
	parameterLinks.payloadKeys = StringItems(option->Find("payloadKeys"));
	return parameterLinks;
}

const ParameterLinks* PointerOf(const std::optional<ParameterLinks>& parameterLinks)
{
	return parameterLinks ? &*parameterLinks : nullptr;
}

/** A bridge on the hosts of the vectors file with this option. */
Options VectorOptions(const std::optional<ParameterLinks>& parameterLinks)
{
	Options options;
	options.allowedHosts = StringItems(VectorsFile().Find("allowedHosts"));
	options.parameterLinks = parameterLinks;
	return options;
}

/** What resolve is asked for `lookup`, encoded as the SDK encodes `url`. */
std::string ResolveUrlOf(const std::string& lookup)
{
	return kResolvePrefix + PercentEncode(lookup);
}

const std::string kPathBase = "https://my-app.strybok.link/p";

/** The lookup of `url` on `my-app.strybok.link` with these keys; `<not ours>` when it is not ours. */
std::string LookupWith(const std::vector<std::string>& payloadKeys, const std::string& url)
{
	ParameterLinks parameterLinks;
	parameterLinks.payloadKeys = payloadKeys;
	std::string lookup;
	return BuildLookupUrl(url, Hosts(), &parameterLinks, lookup) ? lookup : std::string("<not ours>");
}

} // namespace

TEST(ParameterLinks, TheVectorsFileHasCasesOfBothKinds)
{
	EXPECT_MSG(VectorsFile().type == json::Type::Object, VectorsPath());
	const std::vector<std::string> hosts = StringItems(VectorsFile().Find("allowedHosts"));
	EXPECT_EQ(hosts.size(), Count(2));
	EXPECT(VectorList("lookups") != nullptr && !VectorList("lookups")->items.empty());
	EXPECT(VectorList("configurations") != nullptr && !VectorList("configurations")->items.empty());
}

TEST(ParameterLinks, EveryLookupVector)
{
	const json::Value* lookups = VectorList("lookups");
	EXPECT(lookups != nullptr);
	if (lookups == nullptr)
	{
		return;
	}
	const std::vector<std::string> hosts = NormalizeHosts(StringItems(VectorsFile().Find("allowedHosts")));
	std::size_t ran = 0;
	for (const json::Value& entry : lookups->items)
	{
		const std::string name = StringMember(entry, "name");
		const std::string url = StringMember(entry, "url");
		const json::Value* expectedValue = entry.Find("lookup");
		const bool ours = expectedValue != nullptr && expectedValue->type == json::Type::String;
		const std::string expected = ours ? expectedValue->text : std::string();
		const std::optional<ParameterLinks> option = OptionOf(entry);

		// The rules themselves.
		std::string lookup;
		EXPECT_MSG(BuildLookupUrl(url, hosts, PointerOf(option), lookup) == ours, name);
		if (ours)
		{
			EXPECT_MSG(lookup == expected, name + ": sent " + lookup);
		}
		EXPECT_MSG(IsStrybokUrl(url, hosts, PointerOf(option)) == ours, name);

		// Resolve(url).
		{
			Bridge b(VectorOptions(option));
			EXPECT_MSG(b.sdk->IsStrybokUrl(url) == ours, name);
			const Bridge::ResolveOutcome outcome = b.Resolve(url);
			EXPECT_MSG(outcome.called, name);
			if (ours)
			{
				EXPECT_MSG(outcome.ok, name);
				EXPECT_MSG(b.api.resolves.size() == 1 && b.api.resolves[0].url == ResolveUrlOf(expected), name);
				EXPECT_MSG(outcome.link.uri == url, name);
			}
			else
			{
				EXPECT_MSG(!outcome.ok && outcome.error.kind == ErrorKind::UnsupportedLink, name);
				EXPECT_MSG(b.api.resolves.empty(), name);
			}
		}

		// The automatic flow.
		{
			Bridge b(VectorOptions(option));
			b.Listen();
			EXPECT_MSG(b.Initialize(), name);
			b.sdk->HandleUrl(url);
			b.loop.Settle();
			EXPECT_MSG(b.errors.empty(), name);
			if (ours)
			{
				EXPECT_MSG(b.api.resolves.size() == 1 && b.api.resolves[0].url == ResolveUrlOf(expected), name);
				EXPECT_MSG(b.received.size() == 1, name);
			}
			else
			{
				EXPECT_MSG(b.api.resolves.empty(), "dropped without a request: " + name);
				EXPECT_MSG(b.received.empty(), name);
			}
		}
		++ran;
	}
	EXPECT_EQ(ran, lookups->items.size());
}

TEST(ParameterLinks, EveryConfigurationVector)
{
	const json::Value* configurations = VectorList("configurations");
	EXPECT(configurations != nullptr);
	if (configurations == nullptr)
	{
		return;
	}
	for (const json::Value& entry : configurations->items)
	{
		const std::string name = StringMember(entry, "name");
		const json::Value* validValue = entry.Find("valid");
		const bool valid = validValue != nullptr && validValue->type == json::Type::Bool && validValue->boolean;
		// Rejected the way an invalid apiBaseUrl is: by ValidateConfig, which Configure checks.
		Config config;
		config.apiBaseUrl = kApi;
		config.allowedHosts = {kHost};
		config.parameterLinks = OptionOf(entry);
		const std::string problem = ValidateConfig(config);
		EXPECT_MSG(problem.empty() == valid, name + ": " + problem);
		if (config.parameterLinks)
		{
			EXPECT_MSG(ValidateParameterLinks(*config.parameterLinks) == problem, name);
		}
		if (!valid)
		{
			EXPECT_MSG(problem.compare(0, 16, "parameterLinks: ") == 0, name + ": " + problem);
		}
	}
}

TEST(ParameterLinks, TheOptionIsAbsentByDefault)
{
	EXPECT(!Config().parameterLinks.has_value());
	// Without it the rules are those of 0.3.0, URL by URL.
	const json::Value* lookups = VectorList("lookups");
	const std::vector<std::string> hosts = NormalizeHosts(StringItems(VectorsFile().Find("allowedHosts")));
	for (const json::Value& entry : lookups != nullptr ? lookups->items : std::vector<json::Value>())
	{
		const std::string url = StringMember(entry, "url");
		std::string plain;
		std::string unset;
		const bool ours = BuildLookupUrl(url, hosts, plain);
		EXPECT_MSG(BuildLookupUrl(url, hosts, nullptr, unset) == ours, url);
		EXPECT_MSG(plain == unset, url);
		EXPECT_MSG(IsStrybokUrl(url, hosts, nullptr) == IsStrybokUrl(url, hosts), url);
	}
}

TEST(ParameterLinks, PayloadValuesAreCountedInUtf16CodeUnits)
{
	// U+1F600 is 4 bytes of UTF-8 and 2 UTF-16 code units; "é" is 2 bytes and 1 code unit.
	std::string emoji128;
	for (int index = 0; index < 128; ++index)
	{
		emoji128 += "%F0%9F%98%80";
	}
	EXPECT_EQ(LookupWith({"id"}, kPathBase + "?id=" + emoji128), kPathBase + "?id=" + emoji128);
	EXPECT_EQ(LookupWith({"id"}, kPathBase + "?id=" + emoji128 + "%F0%9F%98%80"), kPathBase);
	EXPECT_EQ(LookupWith({"id"}, kPathBase + "?id=" + std::string(254, 'a') + "%F0%9F%98%80"),
		kPathBase + "?id=" + std::string(254, 'a') + "%F0%9F%98%80");
	EXPECT_EQ(LookupWith({"id"}, kPathBase + "?id=" + std::string(255, 'a') + "%F0%9F%98%80"), kPathBase);

	std::string accented256;
	for (int index = 0; index < 256; ++index)
	{
		accented256 += "%C3%A9";
	}
	EXPECT_EQ(LookupWith({"id"}, kPathBase + "?id=" + accented256), kPathBase + "?id=" + accented256);
	EXPECT_EQ(LookupWith({"id"}, kPathBase + "?id=" + accented256 + "%C3%A9"), kPathBase);
}

TEST(ParameterLinks, ValuesThatAreNotUtf8OrAreControlCharactersAreDropped)
{
	for (const char* value : {"%C0%AF", "%E0%80%AF", "%ED%A0%80", "%F4%90%80%80", "%F5%80%80%80", "%80", "%C3", "%E2%82",
			 "%FF", "%C3%28", "%C2%80", "%C2%9F", "%7F", "%00", "\xFF", "%"})
	{
		EXPECT_MSG(LookupWith({"id", "lang"}, kPathBase + "?id=" + value + "&lang=uk") == kPathBase + "?lang=uk", value);
	}
	// U+00A0 is no control character; raw UTF-8 in the query stands for itself.
	EXPECT_EQ(LookupWith({"id"}, kPathBase + "?id=%C2%A0"), kPathBase + "?id=%C2%A0");
	EXPECT_EQ(LookupWith({"id"}, kPathBase + "?id=caf\xC3\xA9"), kPathBase + "?id=caf%C3%A9");
	EXPECT_EQ(LookupWith({"id"}, kPathBase + "?id=%F0%9F%98%80"), kPathBase + "?id=%F0%9F%98%80");
}

TEST(ParameterLinks, AKeyWithoutAValueInALongLink)
{
	// "link" without "=" is a long link whose deep link is empty: not ours.
	EXPECT_EQ(LookupWith({"id"}, "https://my-app.strybok.link/?link"), "<not ours>");
	EXPECT_EQ(LookupWith({"id"}, "https://my-app.strybok.link/?link&id=7"), "<not ours>");
	// "apn" and "ibi" without "=" are sent empty, as with "=", so the server refuses them as the
	// link host does.
	EXPECT_EQ(LookupWith({"id"}, "https://my-app.strybok.link/?link=https%3A%2F%2Fexample.com%2Fp&apn&ibi"),
		"https://my-app.strybok.link/?link=https%3A%2F%2Fexample.com%2Fp&apn=&ibi=");
	// The top-level payload keys of a long link are never sent; those of the deep link are.
	EXPECT_EQ(LookupWith({"id"}, "https://my-app.strybok.link/?id=1&link=https%3A%2F%2Fexample.com%2Fp%3Fid%3D2"),
		"https://my-app.strybok.link/?link=https%3A%2F%2Fexample.com%2Fp%3Fid%3D2");
}

TEST(ParameterLinks, TheRouteAndPayloadComeFromTheAnswerNotFromTheTappedUrl)
{
	Options options;
	options.parameterLinks = ParameterLinks{{"id", "lang"}};
	Bridge b(options);
	b.api.onResolve = [](const HttpRequest&) {
		return Respond(200,
			"{\"linkId\":\"plink_path\",\"appId\":\"app_1\",\"routePath\":\"/catalog/item\","
			"\"payload\":{\"id\":\"from-server\"},\"campaign\":{\"source\":\"mail\"}}");
	};
	b.Listen();
	EXPECT(b.Initialize());
	const std::string tapped = "https://my-app.strybok.link/product/42?id=7&utm_source=x#top";
	b.sdk->HandleUrl(tapped);
	b.loop.Settle();

	EXPECT_EQ(b.received.size(), Count(1));
	if (b.received.size() == 1)
	{
		EXPECT_EQ(b.received[0].uri, tapped);
		EXPECT_EQ(b.received[0].routePath, "/catalog/item"); // never the tapped path
		EXPECT_EQ(b.received[0].payloadJson, "{\"id\":\"from-server\"}");
		EXPECT_EQ(b.received[0].linkId, "plink_path");
		EXPECT_EQ(b.received[0].campaign.source, "mail");
		EXPECT(!b.received[0].isDeferred);
	}
	EXPECT_EQ(b.api.resolves.size(), Count(1));
	if (b.api.resolves.size() == 1)
	{
		EXPECT_EQ(b.api.resolves[0].url, ResolveUrlOf("https://my-app.strybok.link/product/42?id=7&utm_source=x"));
	}

	// The same through Resolve, for a long link.
	const Bridge::ResolveOutcome outcome =
		b.Resolve("https://my-app.strybok.link/?link=https%3A%2F%2Fexample.com%2Fp%3Fid%3D7&apn=com.example.app");
	EXPECT(outcome.ok);
	EXPECT_EQ(outcome.link.routePath, "/catalog/item");
	EXPECT_EQ(outcome.link.payloadJson, "{\"id\":\"from-server\"}");
}

TEST(ParameterLinks, AppOpenCarriesTheLinkIdOfTheAnswer)
{
	Options options;
	options.parameterLinks = ParameterLinks{{"id"}};
	Bridge b(options);
	b.api.onResolve = [](const HttpRequest&) {
		return Respond(200, "{\"linkId\":\"plink_firebase\",\"appId\":\"app_1\",\"routePath\":\"/p\",\"payload\":{}}");
	};
	b.Listen();
	EXPECT(b.Initialize());
	b.sdk->HandleUrl("https://my-app.strybok.link/?link=https%3A%2F%2Fexample.com%2Fp");
	b.loop.Settle();

	EXPECT_EQ(b.api.events.size(), Count(1));
	if (b.api.events.size() == 1)
	{
		json::Value event;
		EXPECT(json::Parse(b.api.events[0].body, event));
		EXPECT_EQ(StringMember(event, "linkId"), "plink_firebase");
		EXPECT_EQ(StringMember(event, "type"), "app_open");
		EXPECT_EQ(StringMember(event, "sdkVersion"), "0.4.0");
		EXPECT(event.Find("campaign") == nullptr);
	}
}

TEST(ParameterLinks, AnAddressThatIsNoLinkIsLinkNotFound)
{
	Options options;
	options.parameterLinks = ParameterLinks{};
	Bridge b(options);
	b.api.onResolve = [](const HttpRequest&) { return Respond(404); };
	b.Listen();
	EXPECT(b.Initialize());
	b.sdk->HandleUrl("https://my-app.strybok.link/no/such/route");
	b.loop.Settle();

	EXPECT_EQ(b.errors.size(), Count(1));
	if (b.errors.size() == 1)
	{
		EXPECT_EQ(b.errors[0].kind, ErrorKind::LinkNotFound);
		EXPECT_EQ(b.errors[0].url, "https://my-app.strybok.link/no/such/route");
	}
	EXPECT(b.received.empty());
	EXPECT_EQ(b.api.resolves.size(), Count(1)); // a 404 is not retried
}

TEST(ParameterLinks, DedupeComparesTheFullUrlAsReceived)
{
	Options options;
	options.parameterLinks = ParameterLinks{{"id"}};
	Bridge b(options);
	b.Listen();
	EXPECT(b.Initialize());
	b.sdk->HandleUrl("https://my-app.strybok.link/product/42?id=7");
	b.sdk->HandleUrl("https://my-app.strybok.link/product/42?id=7"); // one tap reported twice
	b.sdk->HandleUrl("https://my-app.strybok.link/product/42?id=7&ref=x"); // another URL, the same lookup
	b.loop.Settle();

	EXPECT_EQ(b.received.size(), Count(2));
	EXPECT_EQ(b.api.resolves.size(), Count(2));
	for (const HttpRequest& request : b.api.resolves)
	{
		EXPECT_EQ(request.url, ResolveUrlOf("https://my-app.strybok.link/product/42?id=7"));
	}
}

TEST(ParameterLinks, DeferredAnswersKeepTheSlugRule)
{
	for (const char* url : {"https://my-app.strybok.link/product/42", "https://my-app.strybok.link/",
			 "https://my-app.strybok.link/?link=https%3A%2F%2Fexample.com%2Fp"})
	{
		ResolvedLink parsed;
		EXPECT_MSG(!ParseDeferredBody(DeferredBody(url), Hosts(), false, parsed), url);

		Deferred d("android");
		d.config.parameterLinks = ParameterLinks{{"id", "lang"}};
		d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick))};
		const std::string body = DeferredBody(url);
		d.api.onDeferred = [body](const HttpRequest&) { return Respond(200, body); };
		d.Listen();
		EXPECT(d.Start());
		d.loop.Settle();
		EXPECT_EQ(d.Status(), DeferredLinkStatus::Failed);
		EXPECT_MSG(d.received.empty(), url);
		EXPECT_EQ(d.api.deferred.size(), Count(1));
		EXPECT(d.store->Done());
	}

	// The same answer with a slug is delivered, with the option set as well.
	Deferred d("android");
	d.config.parameterLinks = ParameterLinks{{"id", "lang"}};
	d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick))};
	d.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody("https://my-app.strybok.link/Ab3xK9")); };
	d.Listen();
	EXPECT(d.Start());
	d.loop.Settle();
	EXPECT_EQ(d.received.size(), Count(1));
	if (d.received.size() == 1)
	{
		EXPECT(d.received[0].isDeferred);
	}
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Delivered);
}

TEST(ParameterLinks, APastedPathLinkIsNotOurs)
{
	// The page before the App Store copies a stored link's URL; a path link never has a click id.
	Deferred d("ios");
	d.config.parameterLinks = ParameterLinks{{"id"}};
	d.Listen();
	EXPECT(d.Start());
	d.Paste("https://my-app.strybok.link/product/42?strybok_click=" + kClick);
	d.loop.Settle();
	EXPECT_EQ(d.pasted.size(), Count(1));
	if (d.pasted.size() == 1)
	{
		EXPECT_EQ(d.pasted[0], PastedLinkResult::NotOurs);
	}
	EXPECT(d.api.deferred.empty());

	d.Paste(kPasted);
	d.loop.Settle();
	EXPECT_EQ(d.pasted.size(), Count(2));
	if (d.pasted.size() == 2)
	{
		EXPECT_EQ(d.pasted[1], PastedLinkResult::NotFound);
	}
	EXPECT_EQ(d.api.deferred.size(), Count(1));
}

TEST(ParameterLinks, APathLinkTappedFirstSupersedesTheDeferredLink)
{
	// A path link is a normal link of this app: the deferred link is dropped (one launch, one navigation).
	Deferred d("android");
	d.config.parameterLinks = ParameterLinks{{"id"}};
	d.config.installReferrerSources = {Answering(Referrer("strybok_click=" + kClick))};
	d.api.onDeferred = [](const HttpRequest&) { return Respond(200, DeferredBody()); };
	d.Listen();
	EXPECT(d.Start("https://my-app.strybok.link/product/42?id=7"));
	d.loop.Settle();
	EXPECT_EQ(d.Status(), DeferredLinkStatus::Superseded);
	EXPECT(d.api.deferred.empty()); // no request is made
	EXPECT_EQ(d.received.size(), Count(1));
	if (d.received.size() == 1)
	{
		EXPECT(!d.received[0].isDeferred);
	}
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
