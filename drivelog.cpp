#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <string>
#include <utility>
#include <vector>

#include "scssdk_telemetry.h"
#include "common/scssdk_telemetry_common_configs.h"
#include "common/scssdk_telemetry_common_channels.h"
#include "common/scssdk_telemetry_common_gameplay_events.h"
#include "common/scssdk_telemetry_truck_common_channels.h"
#include "common/scssdk_telemetry_trailer_common_channels.h"
#include "common/scssdk_telemetry_job_common_channels.h"

#define UNUSED(x)

static const char *const DRIVELOG_VERSION = "1.9.0";
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

	// One directory per game (ats, eut2), so that a job of one game cannot take over the state of the other.
	data_dir = std::string(DRIVELOG_DATA_DIR) + "/" + version_params->common.game_id;
	const std::string &dir = data_dir;
	mkdir(DRIVELOG_DATA_DIR, 0755);
	mkdir(dir.c_str(), 0755);
	events_file = open_output(dir, "events.jsonl");
	trace_file = open_output(dir, "trace.jsonl");
	if (! events_file || ! trace_file) {
		close_outputs();
		version_params->common.log(SCS_LOG_TYPE_error, ("drivelog: unable to open output files in " + dir).c_str());
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
		version_params->common.log(SCS_LOG_TYPE_error, "drivelog: unable to register event callbacks");
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
	write_line(events_file, "start", ",\"version\":\"" + std::string(DRIVELOG_VERSION) + "\"");
	version_params->common.log(SCS_LOG_TYPE_message, ("drivelog " + std::string(DRIVELOG_VERSION) + ": writing to " + dir).c_str());
	return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void)
{
	close_outputs();
}

void __attribute__ ((destructor)) unload(void)
{
	close_outputs();
}
