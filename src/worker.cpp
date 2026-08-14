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
#include <string>
#include <thread>

namespace {

std::string env(const char *k, const char *fallback = "") {
	const char *v = getenv(k);
	return v ? v : fallback;
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

Reply http_post(const std::string &url, const std::string &body, const std::string &request_id) {
	Reply r;
	CURL *c = curl_easy_init();
	if (!c) {
		return r;
	}
	curl_slist *h = nullptr;
	h = curl_slist_append(h, "Content-Type: application/json");
	const std::string rid = "X-Request-ID: " + request_id;
	h = curl_slist_append(h, rid.c_str());
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
	const std::string take = subst(env("RUNPOD_WEBHOOK_GET_JOB"), "$ID", worker);
	const std::string done_tpl = subst(env("RUNPOD_WEBHOOK_POST_OUTPUT"), "$RUNPOD_POD_ID", worker);

	if (take.empty() || done_tpl.empty()) {
		fprintf(stderr, "worker: RUNPOD_WEBHOOK_GET_JOB / _POST_OUTPUT unset -- not in a worker\n");
		return 1;
	}
	fprintf(stderr, "worker: c++ worker up, id=%s\n", worker.c_str());

	// The heartbeat, on its own thread and started before the first job-take.
	//
	// The Python SDK starts its ping thread first and never explains why, so the
	// ordering is copied rather than reasoned about: a worker that is only known
	// to be alive by the job it has not taken yet is indistinguishable from one
	// that has hung, and this is the only call that says otherwise.
	const std::string ping = subst(env("RUNPOD_WEBHOOK_PING"), "$RUNPOD_POD_ID", worker);
	const std::string api_key = env("RUNPOD_AI_API_KEY");
	const long ping_ms = atol(env("RUNPOD_PING_INTERVAL", "10000").c_str());
	std::thread heartbeat;
	if (!ping.empty()) {
		heartbeat = std::thread([ping, api_key, ping_ms] {
			for (;;) {
				const Reply p = http_get(ping, api_key);
				static long last = -1;
				if (p.status != last) {
					fprintf(stderr, "worker: ping -> %ld\n", p.status);
					last = p.status;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(ping_ms > 0 ? ping_ms : 10000));
			}
		});
		heartbeat.detach();
	} else {
		fprintf(stderr, "worker: RUNPOD_WEBHOOK_PING unset, no heartbeat\n");
	}

	for (;;) {
		const Reply job = http_get(take + "&job_in_progress=0");

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
			fprintf(stderr, "worker: job with no id\n");
			continue;
		}
		fprintf(stderr, "worker: job %s\n", id.c_str());

		// The handler seam. An interactor goes here: `weft_ask(in, command, ...)`
		// with the reply base64'd into the output. Echo proves the wire.
		const std::string output = "{\"language\":\"c++\",\"worker\":\"" + worker +
				"\",\"echo\":" + (input.empty() ? "null" : input) + "}";

		const std::string done = subst(done_tpl, "$ID", id) + "&isStream=false";
		const Reply posted = http_post(done, "{\"output\":" + output + "}", id);
		fprintf(stderr, "worker: posted %s -> %ld\n", id.c_str(), posted.status);
	}
}
