// A RunPod Serverless worker, in C++.
//
// RunPod ships client SDKs for Python, JavaScript and Go, and handler functions
// for Python. There is no C++ worker, and the wire a worker speaks is not
// documented -- what is here was derived from `runpod/runpod-python`
// (`rp_job.py`, `rp_http.py`) and is what this proves.
//
// This file is the protocol and nothing else. The handler it calls is a seam:
// the echo below exists so the worker API can be proved before an interactor
// with 14 GB of weights is attached to it, because those are two different
// questions and answering them together answers neither.
//
// SPDX-License-Identifier: Apache-2.0

#include <curl/curl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

namespace {

std::string env(const char *k, const char *fallback = "") {
	const char *v = getenv(k);
	return v ? v : fallback;
}

// ── The log ───────────────────────────────────────────────────────────────────
//
// RunPod shows worker logs only in its web console: there is no logs path in the
// REST API (checked against /v1/openapi.json, 23 paths) and GraphQL has no
// `workers` field on Endpoint. A serverless worker also has no inbound HTTP, so
// it cannot serve its own records the way a pod can.
//
// So it writes them to the network volume instead, and something with a proxy
// hostname reads them from there. Same OTel NDJSON record shape as
// `interactor-seethrough-ggml`'s `entrypoint.sh`, so one reader serves both.
//
// stderr keeps every line too: when the console *is* reachable that is still the
// first place anyone looks, and a log that exists in only one place is a log that
// is missing whenever that place is the unreachable one.
const char *LOGF = nullptr;

int severity_number(const char *sev) {
	if (!strcmp(sev, "DEBUG")) return 5;
	if (!strcmp(sev, "INFO")) return 9;
	if (!strcmp(sev, "WARN")) return 13;
	if (!strcmp(sev, "ERROR")) return 17;
	if (!strcmp(sev, "FATAL")) return 21;
	return 0;
}

std::string json_escape(const std::string &s) {
	std::string o;
	for (char ch : s) {
		switch (ch) {
		case '"': o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\n': o += "\\n"; break;
		case '\r': o += "\\r"; break;
		case '\t': o += "\\t"; break;
		default:
			if ((unsigned char)ch < 0x20) {
				char b[8];
				snprintf(b, sizeof b, "\\u%04x", ch);
				o += b;
			} else {
				o += ch;
			}
		}
	}
	return o;
}

void emit(const char *sev, const std::string &body, const std::string &event = "") {
	fprintf(stderr, "worker: %s\n", body.c_str());
	fflush(stderr);
	if (!LOGF) {
		return;
	}
	// Opened and closed per record, and flushed: a worker that is killed between
	// jobs still leaves every line it wrote, which is the whole point of writing
	// them somewhere durable.
	FILE *f = fopen(LOGF, "a");
	if (!f) {
		return;
	}
	char ts[64];
	const time_t now = time(nullptr);
	struct tm tmv;
#ifdef _WIN32
	gmtime_s(&tmv, &now);
#else
	gmtime_r(&now, &tmv);
#endif
	strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tmv);
	fprintf(f,
			"{\"Timestamp\":\"%s\",\"SeverityText\":\"%s\",\"SeverityNumber\":%d,"
			"\"Body\":\"%s\",\"Attributes\":{\"event.name\":\"%s\"},"
			"\"Resource\":{\"service.name\":\"transport-runpod\",\"runpod.pod.id\":\"%s\"}}\n",
			ts, sev, severity_number(sev), json_escape(body).c_str(),
			json_escape(event).c_str(), env("RUNPOD_POD_ID", "unknown").c_str());
	fclose(f);
}

// `$ID` means the worker id in the job-take URL and the *job* id in the output
// URL. The same token meaning two things is the sharpest edge in this protocol,
// so replacement is always explicit about which one is being substituted.
std::string subst(std::string s, const std::string &token, const std::string &with) {
	for (size_t at; (at = s.find(token)) != std::string::npos;) {
		s.replace(at, token.size(), with);
	}
	return s;
}

size_t sink(char *p, size_t sz, size_t n, void *out) {
	((std::string *)out)->append(p, sz * n);
	return sz * n;
}

struct Reply {
	long status = 0;
	std::string body;
};

Reply http_get(const std::string &url, const std::string &authorization = "") {
	Reply r;
	CURL *c = curl_easy_init();
	if (!c) {
		return r;
	}
	curl_slist *h = nullptr;
	if (!authorization.empty()) {
		const std::string a = "Authorization: " + authorization;
		h = curl_slist_append(h, a.c_str());
		curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	}
	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
	if (curl_easy_perform(c) == CURLE_OK) {
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
	}
	if (h) {
		curl_slist_free_all(h);
	}
	curl_easy_cleanup(c);
	return r;
}

Reply http_post(const std::string &url, const std::string &body, const std::string &request_id,
		const std::string &authorization) {
	Reply r;
	CURL *c = curl_easy_init();
	if (!c) {
		return r;
	}
	curl_slist *h = nullptr;
	h = curl_slist_append(h, "Content-Type: application/json");
	const std::string rid = "X-Request-ID: " + request_id;
	h = curl_slist_append(h, rid.c_str());
	if (!authorization.empty()) {
		const std::string a = "Authorization: " + authorization;
		h = curl_slist_append(h, a.c_str());
	}
	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
	if (curl_easy_perform(c) == CURLE_OK) {
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
	}
	curl_slist_free_all(h);
	curl_easy_cleanup(c);
	return r;
}

// Enough JSON to find `"id"` and to lift `"input"` out whole. A parser is not
// wanted here: `input` is handed on as the caller wrote it, so re-encoding it
// through a model of JSON would only be a way to change it by accident.
std::string json_string(const std::string &s, const std::string &key) {
	const std::string k = "\"" + key + "\"";
	size_t at = s.find(k);
	if (at == std::string::npos) {
		return "";
	}
	at = s.find('"', s.find(':', at + k.size()) + 1);
	if (at == std::string::npos) {
		return "";
	}
	const size_t end = s.find('"', at + 1);
	return end == std::string::npos ? "" : s.substr(at + 1, end - at - 1);
}

std::string json_value(const std::string &s, const std::string &key) {
	const std::string k = "\"" + key + "\"";
	size_t at = s.find(k);
	if (at == std::string::npos) {
		return "";
	}
	at = s.find(':', at + k.size());
	if (at == std::string::npos) {
		return "";
	}
	at++;
	while (at < s.size() && isspace((unsigned char)s[at])) {
		at++;
	}
	if (at >= s.size()) {
		return "";
	}
	if (s[at] != '{' && s[at] != '[') {
		const size_t end = s.find_first_of(",}", at);
		return s.substr(at, end == std::string::npos ? std::string::npos : end - at);
	}
	const char open = s[at], close = open == '{' ? '}' : ']';
	int depth = 0;
	bool in_str = false;
	for (size_t i = at; i < s.size(); i++) {
		const char ch = s[i];
		if (in_str) {
			if (ch == '\\') {
				i++;
			} else if (ch == '"') {
				in_str = false;
			}
			continue;
		}
		if (ch == '"') {
			in_str = true;
		} else if (ch == open) {
			depth++;
		} else if (ch == close && --depth == 0) {
			return s.substr(at, i - at + 1);
		}
	}
	return "";
}

} // namespace

int main() {
	curl_global_init(CURL_GLOBAL_DEFAULT);

	const std::string worker = env("RUNPOD_POD_ID", "local");
	// Every call a worker makes carries this, not just the ping: the Python SDK
	// puts it on the session (`http_client.get_auth_header`), so job-take and the
	// result post are authenticated too. A job-take without it is answered as
	// though there were no work, which is indistinguishable from an idle queue.
	const std::string api_key = env("RUNPOD_AI_API_KEY");
	const std::string take = subst(env("RUNPOD_WEBHOOK_GET_JOB"), "$ID", worker);
	const std::string done_tpl = subst(env("RUNPOD_WEBHOOK_POST_OUTPUT"), "$RUNPOD_POD_ID", worker);

	// Before anything that could fail: the first record is the one worth having,
	// and a log opened after the check cannot hold the check's own verdict.
	static std::string logf = env("LOGF", "/runpod-volume/transport-runpod.ndjson");
	LOGF = logf.empty() ? nullptr : logf.c_str();
	emit("INFO", "c++ worker up, id=" + worker + ", log=" + logf, "worker.start");
	emit("INFO", std::string("env: GET_JOB=") + (take.empty() ? "unset" : "set") +
					" POST_OUTPUT=" + (done_tpl.empty() ? "unset" : "set") +
					" PING=" + (env("RUNPOD_WEBHOOK_PING").empty() ? "unset" : "set") +
					" API_KEY=" + (api_key.empty() ? "unset" : "set"),
			"worker.env");

	if (take.empty() || done_tpl.empty()) {
		emit("FATAL", "RUNPOD_WEBHOOK_GET_JOB / _POST_OUTPUT unset -- not in a worker", "worker.env");
		// Not an exit: a container that dies takes the explanation with it, and
		// this record is the explanation. Idle so the volume keeps it.
		for (;;) {
			std::this_thread::sleep_for(std::chrono::seconds(30));
		}
	}

	// The heartbeat, on its own thread and started before the first job-take.
	//
	// The Python SDK starts its ping thread first and never explains why, so the
	// ordering is copied rather than reasoned about: a worker that is only known
	// to be alive by the job it has not taken yet is indistinguishable from one
	// that has hung, and this is the only call that says otherwise.
	const std::string ping = subst(env("RUNPOD_WEBHOOK_PING"), "$RUNPOD_POD_ID", worker);
	const long ping_ms = atol(env("RUNPOD_PING_INTERVAL", "10000").c_str());
	std::thread heartbeat;
	if (!ping.empty()) {
		heartbeat = std::thread([ping, api_key, ping_ms] {
			for (;;) {
				const Reply p = http_get(ping, api_key);
				static long last = -1;
				if (p.status != last) {
					emit("INFO", "ping -> " + std::to_string(p.status), "worker.ping");
					last = p.status;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(ping_ms > 0 ? ping_ms : 10000));
			}
		});
		heartbeat.detach();
	} else {
		emit("WARN", "RUNPOD_WEBHOOK_PING unset, no heartbeat", "worker.ping");
	}

	long seen = -1;
	long seen = -1;
	for (;;) {
		const Reply job = http_get(take + "&job_in_progress=0", api_key);
		if (job.status != seen) {
			// Every distinct status once, 204 included. A stuck queue with a ready
			// worker is exactly the case where "no job" is the interesting answer.
			emit("INFO", "job-take -> " + std::to_string(job.status) + " " + job.body.substr(0, 300),
					"worker.take");
			seen = job.status;
		}
		if (job.status != seen) {
			// Every distinct status once, 204 included. A stuck queue with a ready
			// worker is exactly the case where "no job" is the interesting answer.
			emit("INFO", "job-take -> " + std::to_string(job.status) + " " + job.body.substr(0, 300),
					"worker.take");
			seen = job.status;
		}

		// 204 is "no job" and 400 is the same answer when FlashBoot is on.
		// Neither is a failure, and a worker that logged them as one would fill
		// its log with the common case.
		if (job.status == 204 || job.status == 400) {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			continue;
		}
		if (job.status == 429) {
			std::this_thread::sleep_for(std::chrono::seconds(2));
			continue;
		}
		if (job.status != 200) {
			// The body is logged for unexpected statuses only. A worker that
			// printed every 204 would bury the one response that explains a
			// stuck queue, which is the failure this logging exists for.
			fprintf(stderr, "worker: job-take %ld: %.200s\n", job.status, job.body.c_str());
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;
		}

		const std::string id = json_string(job.body, "id");
		const std::string input = json_value(job.body, "input");
		if (id.empty()) {
			emit("ERROR", "job with no id: " + job.body.substr(0, 300), "worker.job");
			continue;
		}
		emit("INFO", "job " + id, "worker.job");

		// The handler seam. An interactor goes here: `weft_ask(in, command, ...)`
		// with the reply base64'd into the output. Echo proves the wire.
		const std::string output = "{\"language\":\"c++\",\"worker\":\"" + worker +
				"\",\"echo\":" + (input.empty() ? "null" : input) + "}";

		const std::string done = subst(done_tpl, "$ID", id) + "&isStream=false";
		const Reply posted = http_post(done, "{\"output\":" + output + "}", id, api_key);
		emit("INFO", "posted " + id + " -> " + std::to_string(posted.status), "worker.done");
	}
}
