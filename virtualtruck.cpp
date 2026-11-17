#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <curl/curl.h>

#include "scssdk_telemetry.h"
#include "common/scssdk_telemetry_common_configs.h"
#include "common/scssdk_telemetry_common_channels.h"
#include "common/scssdk_telemetry_common_gameplay_events.h"
#include "common/scssdk_telemetry_truck_common_channels.h"
#include "common/scssdk_telemetry_trailer_common_channels.h"
#include "common/scssdk_telemetry_job_common_channels.h"

#define UNUSED(x)

static const char *const VIRTUALTRUCK_VERSION = "3.0.0";
static const time_t TRACE_INTERVAL_S = 5;

static std::string data_dir;
static FILE *events_file = NULL;
static FILE *trace_file = NULL;
static bool paused = true;
static time_t last_trace = 0;

struct job_state_t
{
	const char *config_id;
	const char *delivered_event;
	const char *cancelled_event;
	// Attributes of the last non-empty configuration; kept after the job ends so that
	// the delivered/cancelled event can be logged together with the job it belongs to.
	std::string last;
	bool active;
	std::string id;
	std::string identity;
	// Unfinished jobs as (id, identity), kept in a state file: a job continued after a game restart
	// or resumed after being suspended (external jobs) keeps its id.
	std::vector<std::pair<std::string, std::string>> open;
};

static job_state_t job_states[] = {
	{SCS_TELEMETRY_CONFIG_job, SCS_TELEMETRY_GAMEPLAY_EVENT_job_delivered, SCS_TELEMETRY_GAMEPLAY_EVENT_job_cancelled, "", false, "", "", {}},
	{SCS_TELEMETRY_CONFIG_car_job, SCS_TELEMETRY_GAMEPLAY_EVENT_car_job_delivered, SCS_TELEMETRY_GAMEPLAY_EVENT_car_job_cancelled, "", false, "", "", {}},
	// SDK 1.16 does not document bus job events; "bus_job.completed" seen in ETS2 1.62, the cancel event is unknown yet.
	{SCS_TELEMETRY_CONFIG_bus_job, "bus_job.completed", "", "", false, "", "", {}},
};
static const size_t JOB_STATE_COUNT = sizeof(job_states) / sizeof(job_states[0]);

// Attributes the game changes during a job: cargo.loaded with an own trailer (loading at the source company),
// actual departure/arrival times and the current stage of a bus job.
static const char *const JOB_IDENTITY_EXCLUDED[] = {
	"cargo.loaded",
	"bus.stages.current", "source_stop.departure_time.actual", "destination_stop.arrival_time.actual",
	NULL
};

static std::string last_truck;

// The truck and trailer configurations also carry per-wheel and other technical attributes we do not need.
static const char *const TRUCK_ATTRIBUTES[] = {"brand_id", "brand", "id", "name", "fuel.capacity", "license.plate", "license.plate.country", NULL};
static const char *const TRAILER_ATTRIBUTES[] = {"id", "cargo.accessory.id", "brand_id", "brand", "name", "chain.type", "body.type", "license.plate", "license.plate.country", NULL};

// Indexed by the trailer position in the combination (trailer.0 is hooked to the truck); empty when there is no such trailer.
static std::string last_trailers[SCS_TELEMETRY_trailers_count];

template <typename T> struct optional_t
{
	bool available;
	T value;
};

struct truck_state_t
{
	optional_t<scs_u32_t> game_time;
	bool placement_available;
	scs_value_dplacement_t placement;
	float speed;
	float fuel;
	optional_t<float> fuel_consumption;
	float odometer;
	optional_t<float> speed_limit;
	optional_t<scs_s32_t> rest_stop;
	optional_t<scs_s32_t> mandatory_break;
	optional_t<bool> trailer_connected;
	optional_t<float> cargo_damage;
	optional_t<float> wear_engine;
	optional_t<float> wear_transmission;
	optional_t<float> wear_cabin;
	optional_t<float> wear_chassis;
	optional_t<float> wear_wheels;
	optional_t<float> trailer_wear_body;
	optional_t<float> trailer_wear_chassis;
	optional_t<float> trailer_wear_wheels;
} truck;

// Trace fields (0.0 – 1.0) and the channels they come from; trailer channels are of the first trailer.
static const struct
{
	const char *field;
	const char *channel;
	optional_t<float> *value;
} DAMAGE_CHANNELS[] = {
	{"cargo_damage", SCS_TELEMETRY_JOB_CHANNEL_cargo_damage, &truck.cargo_damage},
	{"wear_engine", SCS_TELEMETRY_TRUCK_CHANNEL_wear_engine, &truck.wear_engine},
	{"wear_transmission", SCS_TELEMETRY_TRUCK_CHANNEL_wear_transmission, &truck.wear_transmission},
	{"wear_cabin", SCS_TELEMETRY_TRUCK_CHANNEL_wear_cabin, &truck.wear_cabin},
	{"wear_chassis", SCS_TELEMETRY_TRUCK_CHANNEL_wear_chassis, &truck.wear_chassis},
	{"wear_wheels", SCS_TELEMETRY_TRUCK_CHANNEL_wear_wheels, &truck.wear_wheels},
	{"trailer_wear_body", SCS_TELEMETRY_TRAILER_CHANNEL_wear_body, &truck.trailer_wear_body},
	{"trailer_wear_chassis", SCS_TELEMETRY_TRAILER_CHANNEL_wear_chassis, &truck.trailer_wear_chassis},
	{"trailer_wear_wheels", SCS_TELEMETRY_TRAILER_CHANNEL_wear_wheels, &truck.trailer_wear_wheels},
};

// JSON output.

static void append_json_string(std::string &out, const char *s)
{
	out += '"';
	for (; *s; ++s) {
		const unsigned char c = static_cast<unsigned char>(*s);
		if (c == '"' || c == '\\') {
			out += '\\';
			out += static_cast<char>(c);
		}
		else if (c < 0x20) {
			char buf[8];
			snprintf(buf, sizeof(buf), "\\u%04x", c);
			out += buf;
		}
		else {
			out += static_cast<char>(c);
		}
	}
	out += '"';
}

static void append_json_number(std::string &out, const double value)
{
	if (! isfinite(value)) {
		out += "null";
		return;
	}
	char buf[32];
	snprintf(buf, sizeof(buf), "%.10g", value);
	out += buf;
}

static void append_json_value(std::string &out, const scs_value_t &value)
{
	char buf[32];
	switch (value.type) {
		case SCS_VALUE_TYPE_bool:
			out += value.value_bool.value ? "true" : "false";
			break;
		case SCS_VALUE_TYPE_s32:
			snprintf(buf, sizeof(buf), "%d", static_cast<int>(value.value_s32.value));
			out += buf;
			break;
		case SCS_VALUE_TYPE_u32:
			snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(value.value_u32.value));
			out += buf;
			break;
		case SCS_VALUE_TYPE_s64:
			snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value.value_s64.value));
			out += buf;
			break;
		case SCS_VALUE_TYPE_u64:
			snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value.value_u64.value));
			out += buf;
			break;
		case SCS_VALUE_TYPE_float:
			append_json_number(out, value.value_float.value);
			break;
		case SCS_VALUE_TYPE_double:
			append_json_number(out, value.value_double.value);
			break;
		case SCS_VALUE_TYPE_string:
			append_json_string(out, value.value_string.value);
			break;
		default:
			out += "null";
			break;
	}
}

static bool is_listed(const char *const name, const char *const *list)
{
	for (; *list; ++list) {
		if (strcmp(name, *list) == 0) {
			return true;
		}
	}
	return false;
}

static std::string attributes_json(const scs_named_value_t *const attributes, const char *const *const only = NULL, const char *const *const except = NULL)
{
	std::string out = "{";
	for (const scs_named_value_t *current = attributes; current->name; ++current) {
		if ((only && ! is_listed(current->name, only)) || (except && is_listed(current->name, except))) {
			continue;
		}
		if (out.size() > 1) {
			out += ',';
		}
		std::string name = current->name;
		if (current->index != SCS_U32_NIL) {
			name += "[" + std::to_string(current->index) + "]";
		}
		append_json_string(out, name.c_str());
		out += ':';
		append_json_value(out, current->value);
	}
	out += '}';
	return out;
}

static void append_json_optional(std::string &out, const optional_t<bool> &value)
{
	out += value.available ? (value.value ? "true" : "false") : "null";
}

template <typename T> static void append_json_optional(std::string &out, const optional_t<T> &value)
{
	if (value.available) {
		append_json_number(out, value.value);
	}
	else {
		out += "null";
	}
}

static void write_line(FILE *const file, const char *const type, const std::string &fields)
{
	if (! file) {
		return;
	}
	// The configuration events arrive before the first game.time value, so game_time can be null.
	std::string line = "{\"wall\":" + std::to_string(time(NULL)) + ",\"game_time\":";
	append_json_optional(line, truck.game_time);
	line += ",\"type\":\"" + std::string(type) + "\"" + fields + "}\n";
	fputs(line.c_str(), file);
	fflush(file);
}

// Event handling.

SCSAPI_VOID telemetry_pause(const scs_event_t event, const void *const UNUSED(event_info), const scs_context_t UNUSED(context))
{
	paused = (event == SCS_TELEMETRY_EVENT_paused);
}

SCSAPI_VOID telemetry_frame_end(const scs_event_t UNUSED(event), const void *const UNUSED(event_info), const scs_context_t UNUSED(context))
{
	const time_t now = time(NULL);
	if (paused || ! truck.placement_available || now - last_trace < TRACE_INTERVAL_S) {
		return;
	}
	last_trace = now;

	std::string fields = ",\"x\":";
	append_json_number(fields, truck.placement.position.x);
	fields += ",\"y\":";
	append_json_number(fields, truck.placement.position.y);
	fields += ",\"z\":";
	append_json_number(fields, truck.placement.position.z);
	fields += ",\"speed\":";
	append_json_number(fields, truck.speed);
	fields += ",\"fuel\":";
	append_json_number(fields, truck.fuel);
	fields += ",\"fuel_consumption\":";
	append_json_optional(fields, truck.fuel_consumption);
	fields += ",\"odometer\":";
	append_json_number(fields, truck.odometer);
	fields += ",\"speed_limit\":";
	append_json_optional(fields, truck.speed_limit);
	fields += ",\"rest_stop\":";
	append_json_optional(fields, truck.rest_stop);
	fields += ",\"mandatory_break\":";
	append_json_optional(fields, truck.mandatory_break);
	fields += ",\"trailer_connected\":";
	append_json_optional(fields, truck.trailer_connected);
	for (const auto &damage : DAMAGE_CHANNELS) {
		fields += ",\"" + std::string(damage.field) + "\":";
		append_json_optional(fields, *damage.value);
	}
	write_line(trace_file, "trace", fields);
}

static void log_configuration(const char *const type, const std::string &attributes, std::string &last, const std::string &fields = "")
{
	if (attributes == last) {
		return;
	}
	last = attributes;
	write_line(events_file, type, fields + ",\"attrs\":" + attributes);
}

// Job state files.

static std::string job_state_path(const job_state_t &job)
{
	return data_dir + "/" + job.config_id + ".state";
}

// Two lines per unfinished job: id and identity.
static void load_job_state(job_state_t &job)
{
	job.id.clear();
	job.identity.clear();
	job.open.clear();
	FILE *const file = fopen(job_state_path(job).c_str(), "r");
	if (! file) {
		return;
	}
	char id[64], identity[16384];
	while (fgets(id, sizeof(id), file) && fgets(identity, sizeof(identity), file)) {
		job.open.emplace_back(std::string(id, strcspn(id, "\n")), std::string(identity, strcspn(identity, "\n")));
	}
	fclose(file);
}

static void save_job_state(const job_state_t &job)
{
	if (job.open.empty()) {
		unlink(job_state_path(job).c_str());
		return;
	}
	FILE *const file = fopen(job_state_path(job).c_str(), "w");
	if (file) {
		for (const auto &open : job.open) {
			fprintf(file, "%s\n%s\n", open.first.c_str(), open.second.c_str());
		}
		fclose(file);
	}
}

static void start_job(job_state_t &job, const std::string &identity)
{
	job.identity = identity;
	for (const auto &open : job.open) {
		if (open.second == identity) {
			job.id = open.first;
			return;
		}
	}
	job.id = std::to_string(time(NULL));
	job.open.emplace_back(job.id, identity);
	save_job_state(job);
}

static void end_job(job_state_t &job)
{
	job.active = false;
	for (auto it = job.open.begin(); it != job.open.end(); ++it) {
		if (it->first == job.id) {
			job.open.erase(it);
			break;
		}
	}
	job.id.clear();
	job.identity.clear();
	save_job_state(job);
}

static std::string job_id_json(const job_state_t &job)
{
	std::string out;
	append_json_string(out, job.id.c_str());
	return out;
}

SCSAPI_VOID telemetry_configuration(const scs_event_t UNUSED(event), const void *const event_info, const scs_context_t UNUSED(context))
{
	const struct scs_telemetry_configuration_t *const info = static_cast<const scs_telemetry_configuration_t *>(event_info);
	const bool empty = ! info->attributes->name;
	for (job_state_t &job : job_states) {
		if (strcmp(info->id, job.config_id) == 0) {
			job.active = ! empty;
			if (empty) {
				return;
			}
			const std::string attributes = attributes_json(info->attributes);
			if (attributes == job.last) {
				return;
			}
			const std::string identity = attributes_json(info->attributes, NULL, JOB_IDENTITY_EXCLUDED);
			if (job.id.empty() || identity != job.identity) {
				start_job(job, identity);
			}
			log_configuration(job.config_id, attributes, job.last, ",\"" + std::string(job.config_id) + "_id\":" + job_id_json(job));
			return;
		}
	}
	if (strcmp(info->id, SCS_TELEMETRY_CONFIG_truck) == 0 && ! empty) {
		log_configuration("truck", attributes_json(info->attributes, TRUCK_ATTRIBUTES), last_truck);
		return;
	}
	const size_t prefix = strlen(SCS_TELEMETRY_CONFIG_trailer ".");
	if (strncmp(info->id, SCS_TELEMETRY_CONFIG_trailer ".", prefix) == 0) {
		const int index = atoi(info->id + prefix);
		if (index < 0 || index >= SCS_TELEMETRY_trailers_count) {
			return;
		}
		if (empty) {
			last_trailers[index].clear();
			return;
		}
		log_configuration("trailer", attributes_json(info->attributes, TRAILER_ATTRIBUTES), last_trailers[index], ",\"index\":" + std::to_string(index));
	}
}

SCSAPI_VOID telemetry_gameplay_event(const scs_event_t UNUSED(event), const void *const event_info, const scs_context_t UNUSED(context))
{
	const struct scs_telemetry_gameplay_event_t *const info = static_cast<const scs_telemetry_gameplay_event_t *>(event_info);
	std::string fields = ",\"id\":";
	append_json_string(fields, info->id);
	fields += ",\"attrs\":" + attributes_json(info->attributes);
	bool ended[JOB_STATE_COUNT];
	for (size_t i = 0; i < JOB_STATE_COUNT; ++i) {
		const job_state_t &job = job_states[i];
		ended[i] = strcmp(info->id, job.delivered_event) == 0 || strcmp(info->id, job.cancelled_event) == 0;
		const bool with_job = (job.active || ended[i]) && ! job.last.empty();
		fields += ",\"" + std::string(job.config_id) + "\":" + (with_job ? job.last : std::string("null"));
		fields += ",\"" + std::string(job.config_id) + "_id\":" + (with_job && ! job.id.empty() ? job_id_json(job) : std::string("null"));
	}
	fields += ",\"truck\":" + (last_truck.empty() ? std::string("null") : last_truck);
	fields += ",\"trailers\":[";
	bool first_trailer = true;
	for (const std::string &trailer : last_trailers) {
		if (! trailer.empty()) {
			fields += (first_trailer ? "" : ",") + trailer;
			first_trailer = false;
		}
	}
	fields += "]";
	write_line(events_file, "gameplay", fields);
	for (size_t i = 0; i < JOB_STATE_COUNT; ++i) {
		if (ended[i]) {
			end_job(job_states[i]);
		}
	}
}

// Channel handling.

SCSAPI_VOID telemetry_store_placement(const scs_string_t UNUSED(name), const scs_u32_t UNUSED(index), const scs_value_t *const value, const scs_context_t UNUSED(context))
{
	truck.placement_available = (value != NULL);
	if (value) {
		truck.placement = value->value_dplacement;
	}
}

SCSAPI_VOID telemetry_store_float(const scs_string_t UNUSED(name), const scs_u32_t UNUSED(index), const scs_value_t *const value, const scs_context_t context)
{
	*static_cast<float *>(context) = value->value_float.value;
}

SCSAPI_VOID telemetry_store_optional_u32(const scs_string_t UNUSED(name), const scs_u32_t UNUSED(index), const scs_value_t *const value, const scs_context_t context)
{
	optional_t<scs_u32_t> *const target = static_cast<optional_t<scs_u32_t> *>(context);
	target->available = (value != NULL);
	if (value) {
		target->value = value->value_u32.value;
	}
}

SCSAPI_VOID telemetry_store_optional_s32(const scs_string_t UNUSED(name), const scs_u32_t UNUSED(index), const scs_value_t *const value, const scs_context_t context)
{
	optional_t<scs_s32_t> *const target = static_cast<optional_t<scs_s32_t> *>(context);
	target->available = (value != NULL);
	if (value) {
		target->value = value->value_s32.value;
	}
}

SCSAPI_VOID telemetry_store_optional_bool(const scs_string_t UNUSED(name), const scs_u32_t UNUSED(index), const scs_value_t *const value, const scs_context_t context)
{
	optional_t<bool> *const target = static_cast<optional_t<bool> *>(context);
	target->available = (value != NULL);
	if (value) {
		target->value = value->value_bool.value != 0;
	}
}

SCSAPI_VOID telemetry_store_optional_float(const scs_string_t UNUSED(name), const scs_u32_t UNUSED(index), const scs_value_t *const value, const scs_context_t context)
{
	optional_t<float> *const target = static_cast<optional_t<float> *>(context);
	target->available = (value != NULL);
	if (value) {
		target->value = value->value_float.value;
	}
}

// Upload to the server; the jsonl files are the queue, the server tells the number of the last line it has.

static const int UPLOAD_INTERVAL_S = 10;
static const size_t UPLOAD_MAX_LINES = 1000;

struct upload_file_t
{
	const char *name;
	// -1 until the server has told it.
	long last_seq;
	// Byte offsets of line last_seq and of the line after it, -1 until found.
	long last_line_offset;
	long offset;
	// Line last_seq is sent once more after finding it, so the server checks the file is the one its lines are from.
	bool verified;
	bool stopped;
};

static std::string base_dir;
static std::string game_id;
static std::string server_url;
static std::string server_token;
static upload_file_t upload_files[] = {{"events", -1, -1, -1, false, false}, {"trace", -1, -1, -1, false, false}};
// Heap thread and atomic flag: unload() may run after the static destructors.
static std::thread *uploader = NULL;
static std::atomic<bool> uploader_stop(false);
// Only the uploader thread uses these.
static CURL *curl = NULL;
static bool upload_failing = false;

static void upload_log(const std::string &message)
{
	FILE *const file = fopen((base_dir + "/upload.log").c_str(), "a");
	if (! file) {
		return;
	}
	char stamp[32];
	const time_t now = time(NULL);
	struct tm local;
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime_r(&now, &local));
	fprintf(file, "%s %s: %s\n", stamp, game_id.c_str(), message.c_str());
	fclose(file);
}

// Without both url and token the plugin only writes the files.
static bool read_config(void)
{
	FILE *const file = fopen((base_dir + "/config").c_str(), "r");
	if (! file) {
		return false;
	}
	char buffer[1024];
	while (fgets(buffer, sizeof(buffer), file)) {
		std::string line(buffer);
		while (! line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
			line.pop_back();
		}
		const size_t equals = line.find('=');
		if (equals == std::string::npos) {
			continue;
		}
		const std::string key = line.substr(0, equals);
		const std::string value = line.substr(equals + 1);
		if (key == "url") {
			server_url = value;
			while (! server_url.empty() && server_url.back() == '/') {
				server_url.pop_back();
			}
		}
		else if (key == "token") {
			server_token = value;
		}
	}
	fclose(file);
	return ! server_url.empty() && ! server_token.empty();
}

static size_t append_response(char *const data, const size_t size, const size_t count, void *const response)
{
	static_cast<std::string *>(response)->append(data, size * count);
	return size * count;
}

// Aborts a running request when the game shuts down.
static int abort_on_stop(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	return uploader_stop ? 1 : 0;
}

// HTTP status, 0 when the server is unreachable.
static long http_request(const upload_file_t &file, const std::string *const body, std::string &response)
{
	// Reset keeps the open connection for the next request.
	curl_easy_reset(curl);
	const std::string url = server_url + "/api/ingest/" + game_id + "/" + file.name;
	const std::string authorization = "Authorization: Bearer " + server_token;
	struct curl_slist *headers = curl_slist_append(NULL, authorization.c_str());
	headers = curl_slist_append(headers, "Accept: application/json");
	headers = curl_slist_append(headers, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_on_stop);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	if (body) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
	}
	long status = 0;
	if (curl_easy_perform(curl) == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	}
	curl_slist_free_all(headers);
	return status;
}

static long response_last_seq(const std::string &response)
{
	const size_t at = response.find("\"last_seq\":");
	return at == std::string::npos ? -1 : strtol(response.c_str() + at + 11, NULL, 10);
}

// Logs only the first of repeated failures (e.g. the server is off for the whole drive).
static void upload_failed(const upload_file_t &file, const long status, const std::string &response)
{
	if (! upload_failing) {
		upload_log(std::string(file.name) + ": server unreachable or failing (HTTP " + std::to_string(status) + ") " + response.substr(0, 300) + "; retrying");
		upload_failing = true;
	}
}

static void upload_stopped(upload_file_t &file, const std::string &reason)
{
	upload_log(std::string(file.name) + ": " + reason + "; uploading stopped until the next start of the game");
	file.stopped = true;
}

// Finds the byte offsets of line last_seq; false when the file has fewer lines than the server.
static bool find_last_line(upload_file_t &file, FILE *const input)
{
	char *line = NULL;
	size_t capacity = 0;
	ssize_t length;
	long seq = 0;
	long line_offset = 0;
	long offset = 0;
	fseek(input, 0, SEEK_SET);
	while (seq < file.last_seq && (length = getline(&line, &capacity, input)) > 0 && line[length - 1] == '\n') {
		line_offset = offset;
		offset += length;
		seq++;
	}
	free(line);
	if (seq < file.last_seq) {
		upload_stopped(file, "the server has " + std::to_string(file.last_seq) + " lines, the file only " + std::to_string(seq));
		return false;
	}
	file.last_line_offset = line_offset;
	file.offset = offset;
	file.verified = file.last_seq == 0;
	return true;
}

// Sends the lines after last_seq that end before limit (-1 = the whole file); true when all of them are on the server.
static bool upload_file(upload_file_t &file, const long limit)
{
	if (file.stopped) {
		return false;
	}
	std::string response;
	if (file.last_seq < 0) {
		const long status = http_request(file, NULL, response);
		const long last_seq = response_last_seq(response);
		if (status != 200 || last_seq < 0) {
			if (status == 0 || status >= 500) {
				upload_failed(file, status, response);
			}
			else {
				upload_stopped(file, "the server refused (HTTP " + std::to_string(status) + ") " + response.substr(0, 300));
			}
			return false;
		}
		file.last_seq = last_seq;
		file.offset = -1;
	}
	FILE *const input = fopen((data_dir + "/" + file.name + ".jsonl").c_str(), "r");
	if (! input) {
		return file.last_seq == 0;
	}
	if (file.offset < 0 && ! find_last_line(file, input)) {
		fclose(input);
		return false;
	}
	char *line = NULL;
	size_t capacity = 0;
	ssize_t length;
	bool done = false;
	while (! uploader_stop) {
		const long start = file.verified ? file.offset : file.last_line_offset;
		const long first_seq = file.verified ? file.last_seq + 1 : file.last_seq;
		fseek(input, start, SEEK_SET);
		std::string body = "{\"seq\":" + std::to_string(first_seq) + ",\"lines\":[";
		size_t count = 0;
		long end = start;
		long last_line = start;
		while (count < UPLOAD_MAX_LINES && (length = getline(&line, &capacity, input)) > 0 && line[length - 1] == '\n'
			&& (limit < 0 || end + length <= limit)) {
			body += (count ? "," : "") + std::string(line, length - 1);
			last_line = end;
			end += length;
			count++;
		}
		if (count == 0) {
			done = true;
			break;
		}
		body += "]}";
		response.clear();
		const long status = http_request(file, &body, response);
		if (status == 200) {
			if (upload_failing) {
				upload_log(std::string(file.name) + ": uploading again");
				upload_failing = false;
			}
			file.last_seq = first_seq + static_cast<long>(count) - 1;
			file.last_line_offset = last_line;
			file.offset = end;
			file.verified = true;
		}
		else if (status == 409 && response_last_seq(response) >= 0 && response_last_seq(response) < file.last_seq) {
			// The server has fewer lines than before; send them again from its last one.
			upload_log(std::string(file.name) + ": the server has " + std::to_string(response_last_seq(response)) + " lines instead of " + std::to_string(file.last_seq) + ", sending again");
			file.last_seq = response_last_seq(response);
			if (! find_last_line(file, input)) {
				break;
			}
		}
		else if (status == 0 || status >= 500) {
			// The lines may have arrived; resending them is safe, the server skips equal lines.
			upload_failed(file, status, response);
			break;
		}
		else {
			upload_stopped(file, "the server refused the lines (HTTP " + std::to_string(status) + ") " + response.substr(0, 300));
			break;
		}
	}
	free(line);
	fclose(input);
	return done;
}

static void upload_loop(void)
{
	curl = curl_easy_init();
	while (curl && ! uploader_stop) {
		// Trace written so far: the events before it go first, so a trace point never comes before the truck or job change preceding it.
		struct stat trace;
		const long trace_end = stat((data_dir + "/trace.jsonl").c_str(), &trace) == 0 ? static_cast<long>(trace.st_size) : 0;
		if (upload_file(upload_files[0], -1) && ! uploader_stop) {
			upload_file(upload_files[1], trace_end);
		}
		for (int i = 0; i < UPLOAD_INTERVAL_S * 10 && ! uploader_stop; i++) {
			usleep(100000);
		}
	}
	if (curl) {
		curl_easy_cleanup(curl);
		curl = NULL;
	}
}

static void stop_uploader(void)
{
	if (! uploader) {
		return;
	}
	uploader_stop = true;
	uploader->join();
	delete uploader;
	uploader = NULL;
}

// Plugin lifecycle.

static FILE *open_output(const std::string &dir, const char *const file_name)
{
	return fopen((dir + "/" + file_name).c_str(), "a");
}

static void close_outputs(void)
{
	if (events_file) {
		fclose(events_file);
		events_file = NULL;
	}
	if (trace_file) {
		fclose(trace_file);
		trace_file = NULL;
	}
}

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t *const params)
{
	if (version != SCS_TELEMETRY_VERSION_1_01) {
		return SCS_RESULT_unsupported;
	}
	const scs_telemetry_init_params_v101_t *const version_params = static_cast<const scs_telemetry_init_params_v101_t *>(params);

	const char *const home = getenv("HOME");
	if (! home) {
		version_params->common.log(SCS_LOG_TYPE_error, "virtualtruck: HOME is not set");
		return SCS_RESULT_generic_error;
	}
	base_dir = std::string(home) + "/Library/Application Support/virtualtruck";
	game_id = version_params->common.game_id;
	// One directory per game (ats, eut2), so that a job of one game cannot take over the state of the other.
	data_dir = base_dir + "/data/" + game_id;
	const std::string &dir = data_dir;
	mkdir(base_dir.c_str(), 0755);
	mkdir((base_dir + "/data").c_str(), 0755);
	mkdir(dir.c_str(), 0755);
	events_file = open_output(dir, "events.jsonl");
	trace_file = open_output(dir, "trace.jsonl");
	if (! events_file || ! trace_file) {
		close_outputs();
		version_params->common.log(SCS_LOG_TYPE_error, ("virtualtruck: unable to open output files in " + dir).c_str());
		return SCS_RESULT_generic_error;
	}

	const bool events_registered =
		(version_params->register_for_event(SCS_TELEMETRY_EVENT_frame_end, telemetry_frame_end, NULL) == SCS_RESULT_ok) &&
		(version_params->register_for_event(SCS_TELEMETRY_EVENT_paused, telemetry_pause, NULL) == SCS_RESULT_ok) &&
		(version_params->register_for_event(SCS_TELEMETRY_EVENT_started, telemetry_pause, NULL) == SCS_RESULT_ok) &&
		(version_params->register_for_event(SCS_TELEMETRY_EVENT_configuration, telemetry_configuration, NULL) == SCS_RESULT_ok) &&
		(version_params->register_for_event(SCS_TELEMETRY_EVENT_gameplay, telemetry_gameplay_event, NULL) == SCS_RESULT_ok)
	;
	if (! events_registered) {
		close_outputs();
		version_params->common.log(SCS_LOG_TYPE_error, "virtualtruck: unable to register event callbacks");
		return SCS_RESULT_generic_error;
	}

	memset(&truck, 0, sizeof(truck));
	version_params->register_for_channel(SCS_TELEMETRY_CHANNEL_game_time, SCS_U32_NIL, SCS_VALUE_TYPE_u32, SCS_TELEMETRY_CHANNEL_FLAG_no_value, telemetry_store_optional_u32, &truck.game_time);
	version_params->register_for_channel(SCS_TELEMETRY_CHANNEL_next_rest_stop, SCS_U32_NIL, SCS_VALUE_TYPE_s32, SCS_TELEMETRY_CHANNEL_FLAG_no_value, telemetry_store_optional_s32, &truck.rest_stop);
	version_params->register_for_channel(SCS_TELEMETRY_CHANNEL_next_mandatory_break, SCS_U32_NIL, SCS_VALUE_TYPE_s32, SCS_TELEMETRY_CHANNEL_FLAG_no_value, telemetry_store_optional_s32, &truck.mandatory_break);
	version_params->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_navigation_speed_limit, SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_no_value, telemetry_store_optional_float, &truck.speed_limit);
	// The first trailer is the one hooked to the truck, also in double and triple combinations.
	version_params->register_for_channel(SCS_TELEMETRY_TRAILER_CHANNEL_connected, SCS_U32_NIL, SCS_VALUE_TYPE_bool, SCS_TELEMETRY_CHANNEL_FLAG_no_value, telemetry_store_optional_bool, &truck.trailer_connected);
	for (const auto &damage : DAMAGE_CHANNELS) {
		version_params->register_for_channel(damage.channel, SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_no_value, telemetry_store_optional_float, damage.value);
	}
	version_params->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, SCS_U32_NIL, SCS_VALUE_TYPE_dplacement, SCS_TELEMETRY_CHANNEL_FLAG_no_value, telemetry_store_placement, NULL);
	version_params->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, telemetry_store_float, &truck.speed);
	version_params->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_fuel, SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, telemetry_store_float, &truck.fuel);
	version_params->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_fuel_average_consumption, SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_no_value, telemetry_store_optional_float, &truck.fuel_consumption);
	version_params->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_odometer, SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, telemetry_store_float, &truck.odometer);

	paused = true;
	last_trace = 0;
	for (job_state_t &job : job_states) {
		job.last.clear();
		job.active = false;
		load_job_state(job);
	}
	last_truck.clear();
	for (std::string &trailer : last_trailers) {
		trailer.clear();
	}
	write_line(events_file, "start", ",\"version\":\"" + std::string(VIRTUALTRUCK_VERSION) + "\"");
	version_params->common.log(SCS_LOG_TYPE_message, ("virtualtruck " + std::string(VIRTUALTRUCK_VERSION) + ": writing to " + dir).c_str());

	server_url.clear();
	server_token.clear();
	if (read_config()) {
		for (upload_file_t &file : upload_files) {
			file.last_seq = -1;
			file.last_line_offset = -1;
			file.offset = -1;
			file.verified = false;
			file.stopped = false;
		}
		curl_global_init(CURL_GLOBAL_DEFAULT);
		uploader_stop = false;
		upload_failing = false;
		uploader = new std::thread(upload_loop);
		version_params->common.log(SCS_LOG_TYPE_message, ("virtualtruck: uploading to " + server_url).c_str());
	}
	else {
		version_params->common.log(SCS_LOG_TYPE_message, ("virtualtruck: no url and token in " + base_dir + "/config, not uploading").c_str());
	}
	return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void)
{
	if (uploader) {
		stop_uploader();
		curl_global_cleanup();
	}
	close_outputs();
}

void __attribute__ ((destructor)) unload(void)
{
	stop_uploader();
	close_outputs();
}
