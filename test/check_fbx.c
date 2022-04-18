#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

static void ufbxt_assert_fail_imp(const char *func, const char *file, size_t line, const char *msg)
{
	fprintf(stderr, "%s:%zu: %s(%s) failed\n", file, line, func, msg);
	exit(2);
}

#define ufbxt_assert_fail(file, line, msg) ufbxt_assert_fail_imp("ufbxt_assert_fail", file, line, msg)
#define ufbxt_assert(m_cond) do { if (!(m_cond)) ufbxt_assert_fail_imp("ufbxt_assert", __FILE__, __LINE__, #m_cond); } while (0)
#define ufbx_assert(m_cond) do { if (!(m_cond)) ufbxt_assert_fail_imp("ufbx_assert", __FILE__, __LINE__, #m_cond); } while (0)

bool g_verbose = false;

#include "../ufbx.h"
#include "check_scene.h"
#include "testing_utils.h"

#ifdef _WIN32
int wmain(int argc, wchar_t **wide_argv)
#else
int main(int argc, char **argv)
#endif
{
#ifdef _WIN32
	char **argv = (char**)malloc(sizeof(char*) * argc);
	ufbxt_assert(argv);
	for (int i = 0; i < argc; i++) {
		int res = WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, NULL, 0, NULL, NULL);
		ufbxt_assert(res > 0);
		size_t dst_size = (size_t)res + 1;
		char *dst = (char*)malloc(dst_size);
		ufbxt_assert(dst);
		res = WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, dst, (int)dst_size, NULL, NULL);
		ufbxt_assert(res > 0 && (size_t)res < dst_size);
		argv[i] = dst;
	}
#endif

	const char *path = NULL;
	const char *obj_path = NULL;
	const char *dump_obj_path = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			g_verbose = true;
		} else if (!strcmp(argv[i], "--obj")) {
			if (++i < argc) obj_path = argv[i];
		} else if (!strcmp(argv[i], "--dump-obj")) {
			if (++i < argc) dump_obj_path = argv[i];
		} else {
			path = argv[i];
		}
	}

	if (!path) {
		fprintf(stderr, "Usage: check_fbx <file.fbx>\n");
		return 1;
	}

	ufbx_load_opts opts = { 0 };
	opts.evaluate_skinning = true;
	opts.evaluate_caches = true;
	opts.target_axes = ufbx_axes_right_handed_y_up;
	opts.target_unit_meters = 0.01;

	ufbx_error error;
	ufbx_scene *scene = ufbx_load_file(path, &opts, &error);

	if (!scene) {
		char buf[1024];
		ufbx_format_error(buf, sizeof(buf), &error);
		fprintf(stderr, "%s\n", buf);
		return 1;
	}

	const char *exporters[] = {
		"Unknown",
		"FBX SDK",
		"Blender Binary",
		"Blender ASCII",
		"MotionBuilder",
		"Unity Exporter (from Building Crafter)",
	};

	const char *formats[2][2] = {
		{ "binary", "binary (big-endian)" },
		{ "ascii", "!?!?ascii (big-endian)!?!?" },
	};

	const char *application = scene->metadata.latest_application.name.data;
	if (!application[0]) application = "unknown";

	printf("FBX %u %s via %s %u.%u.%u (%s)\n",
		scene->metadata.version,
		formats[scene->metadata.ascii][scene->metadata.big_endian],
		exporters[scene->metadata.exporter],
		ufbx_version_major(scene->metadata.exporter_version),
		ufbx_version_minor(scene->metadata.exporter_version),
		ufbx_version_patch(scene->metadata.exporter_version),
		application);

	int result = 0;

	if (!strstr(path, "ufbx-unknown")) {
		bool ignore_unknowns = false;
		bool has_unknowns = false;

		for (size_t i = 0; i < scene->unknowns.count; i++) {
			ufbx_unknown *unknown = scene->unknowns.data[i];
			if (strstr(unknown->super_type.data, "MotionBuilder")) continue;
			if (strstr(unknown->type.data, "Container")) continue;
			if (!strcmp(unknown->super_type.data, "Object") && unknown->type.length == 0 && unknown->sub_type.length == 0) continue;
			if (!strcmp(unknown->super_type.data, "PluginParameters")) continue;
			if (!strcmp(unknown->super_type.data, "TimelineXTrack")) continue;
			if (!strcmp(unknown->super_type.data, "GlobalShading")) continue;
			if (!strcmp(unknown->super_type.data, "ControlSetPlug")) continue;
			if (!strcmp(unknown->sub_type.data, "NodeAttribute")) continue;
			if (!strcmp(unknown->type.data, "GroupSelection")) continue;
			if (!strcmp(unknown->name.data, "ADSKAssetReferencesVersion3.0")) {
				ignore_unknowns = true;
			}

			has_unknowns = true;
			fprintf(stderr, "Unknown element: %s/%s/%s : %s\n", unknown->super_type.data, unknown->type.data, unknown->sub_type.data, unknown->name.data);
		}

		if (has_unknowns && !ignore_unknowns) {
			result = 3;
		}
	}

	bool known_unknown = false;
	if (strstr(scene->metadata.creator.data, "kenney")) known_unknown = true;
	if (strstr(scene->metadata.creator.data, "assetforge")) known_unknown = true;
	if (scene->metadata.version < 5800) known_unknown = true;
	ufbxt_assert(scene->metadata.exporter != UFBX_EXPORTER_UNKNOWN || known_unknown);

	ufbxt_check_scene(scene);

	if (obj_path) {
		size_t obj_size;
		void *obj_data = ufbxt_read_file(obj_path, &obj_size);
		if (!obj_data) {
			fprintf(stderr, "Failed to read .obj file: %s\n", obj_path);
			return 1;
		}

		ufbxt_load_obj_opts obj_opts = { 0 };

		ufbxt_obj_file *obj_file = ufbxt_load_obj(obj_data, obj_size, &obj_opts);

		obj_file->normalize_units = true;

		ufbx_scene *state;
		if (obj_file->animation_frame >= 0) {
			double time = (double)obj_file->animation_frame / (double)scene->settings.frames_per_second;
			ufbx_evaluate_opts eval_opts = { 0 };
			eval_opts.evaluate_skinning = true;
			eval_opts.evaluate_caches = true;
			state = ufbx_evaluate_scene(scene, &scene->anim, time, &eval_opts, NULL);
			ufbxt_assert(state);
		} else {
			state = scene;
			ufbx_retain_scene(state);
		}

		if (dump_obj_path) {
			ufbxt_debug_dump_obj_scene(dump_obj_path, state);
			printf("Dumped .obj to %s\n", dump_obj_path);
		}

		ufbxt_diff_error err = { 0 };
		ufbxt_diff_to_obj(state, obj_file, &err, false);

		if (err.num > 0) {
			ufbx_real avg = err.sum / (ufbx_real)err.num;
			printf("Absolute diff: avg %.3g, max %.3g (%zu tests)\n", avg, err.max, err.num);
		}

		ufbx_free_scene(state);
		free(obj_file);
		free(obj_data);
	} else {
		if (dump_obj_path) {
			ufbxt_debug_dump_obj_scene(dump_obj_path, scene);
			printf("Dumped .obj to %s\n", dump_obj_path);
		}
	}

	ufbx_free_scene(scene);

	return result;
}


#include "../ufbx.c"
