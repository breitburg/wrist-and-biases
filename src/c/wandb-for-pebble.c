#include <pebble.h>

// Constants and Data Structures
#define MAX_RUNS 10
#define MAX_METRICS_PER_RUN 18
#define MAX_NAME_LENGTH 32
#define MAX_VALUE_LENGTH 16
#define MAX_HISTORY_POINTS 20

#define PADDING_LEFT 10
#define ANIM_DURATION 200
#define ANIM_SLIDE_DISTANCE 15
#define STATUS_BAR_HEIGHT 16

// Graph drawing constants
#define GRAPH_MARGIN 2
#define GRAPH_PADDING 4
#define INDICATOR_SIZE 10
#define DATA_POINT_SIZE 3

// Animation timing constants
#define SCRUB_FIXED_SCALE 1000
#define SCRUB_ANIM_DURATION 100
#define SCRUB_REPEAT_INTERVAL 150
#define WIGGLE_ANIM_DURATION 300

// Fixed-point arithmetic for value interpolation (4 decimal places)
#define FIXED_POINT_SCALE 10000

typedef enum {
  ScrollDirectionUp,
  ScrollDirectionDown,
} ScrollDirection;

typedef struct {
  char name[MAX_NAME_LENGTH];
  char value[MAX_VALUE_LENGTH];
  int64_t history[MAX_HISTORY_POINTS];  // Fixed-point historical values (64-bit)
  uint8_t history_count;
} WandbMetric;

#define MAX_STATE_LENGTH 16

typedef struct {
  char run_name[MAX_NAME_LENGTH];
  char project_name[MAX_NAME_LENGTH];
  char state[MAX_STATE_LENGTH];
  WandbMetric metrics[MAX_METRICS_PER_RUN];
  uint8_t num_metrics;
} WandbRun;

// App data (persistent)
typedef struct {
  WandbRun runs[MAX_RUNS];
  uint8_t num_runs;
} WandbData;

// UI state (ephemeral)
typedef struct {
  uint8_t selected_run_index;
  uint8_t current_metric_page;
  uint8_t graph_display_page;
  bool loading;
} UIState;

// Main window state
typedef struct {
  Window *window;
  MenuLayer *menu;
  StatusBarLayer *status_bar;
  TextLayer *loading_layer;
  AppTimer *loading_timer;
  bool loading;
} MainWindowState;

// Detail window state
typedef struct {
  Window *window;
  TextLayer *value_layer;
  TextLayer *name_layer;
  Layer *graph_layer;
  Layer *skeleton_layer;
  StatusBarLayer *status_bar;
  TextLayer *pagination_layer;
  AppTimer *loading_timer;
  GRect value_frame;
  GRect name_frame;
  GRect graph_frame;
  Animation *scroll_animation;
} DetailWindowState;

// Scrub mode state
typedef struct {
  bool active;
  uint8_t index;
  int32_t current_index_fixed;
  int32_t from_index_fixed;
  int32_t to_index_fixed;
  Animation *animation;
  char value_buffer[MAX_VALUE_LENGTH];
  AppTimer *repeat_timer;
  int repeat_direction;
  // Bounce/wiggle animation params
  int32_t bounce_target;
  int32_t bounce_return;
  int32_t wiggle_start;
  int32_t wiggle_amount;
} ScrubState;

// Value animation state
typedef struct {
  int32_t from;
  int32_t to;
  int decimals;
  char buffer[MAX_VALUE_LENGTH];
} ValueAnimState;

// Static Variables
static WandbData s_data;
static UIState s_ui;
static MainWindowState s_main;
static DetailWindowState s_detail;
static ScrubState s_scrub;
static ValueAnimState s_value_anim;
static uint8_t s_expected_runs_count;
static uint8_t s_expected_metrics_count;

// Text buffers
#if !defined(PBL_ROUND)
static char s_page_buffer[16];
#endif
static char s_name_buffer[MAX_NAME_LENGTH];

// Helper Functions
static WandbMetric* get_current_metric(void) {
  return &s_data.runs[s_ui.selected_run_index].metrics[s_ui.current_metric_page];
}

static inline void mark_graph_dirty(void) {
  if (s_detail.graph_layer) layer_mark_dirty(s_detail.graph_layer);
}

static int32_t lerp_fixed(int32_t from, int32_t to, AnimationProgress progress) {
  return from + (int32_t)(((int64_t)progress * (to - from)) / ANIMATION_NORMALIZED_MAX);
}

// Menu section helpers - sections correspond to unique states
// Returns the state string for a given section index (based on first occurrence order)
static const char* get_state_for_section(uint16_t section) {
  uint8_t unique_count = 0;
  for (int i = 0; i < s_data.num_runs; i++) {
    // Check if this is the first occurrence of this state
    bool is_first = true;
    for (int j = 0; j < i; j++) {
      if (strcmp(s_data.runs[j].state, s_data.runs[i].state) == 0) {
        is_first = false;
        break;
      }
    }
    if (is_first) {
      if (unique_count == section) return s_data.runs[i].state;
      unique_count++;
    }
  }
  return NULL;
}

static uint8_t count_unique_states(void) {
  uint8_t count = 0;
  for (int i = 0; i < s_data.num_runs; i++) {
    bool is_first = true;
    for (int j = 0; j < i; j++) {
      if (strcmp(s_data.runs[j].state, s_data.runs[i].state) == 0) {
        is_first = false;
        break;
      }
    }
    if (is_first) count++;
  }
  return count;
}

static uint8_t count_runs_with_state(const char *state) {
  uint8_t count = 0;
  for (int i = 0; i < s_data.num_runs; i++) {
    if (strcmp(s_data.runs[i].state, state) == 0) count++;
  }
  return count;
}

static int8_t get_run_index_for_section_row(uint16_t section, uint16_t row) {
  const char *state = get_state_for_section(section);
  if (!state) return -1;

  uint8_t found = 0;
  for (int i = 0; i < s_data.num_runs; i++) {
    if (strcmp(s_data.runs[i].state, state) == 0) {
      if (found == row) return i;
      found++;
    }
  }
  return -1;
}

// Detail Window - Display Updates
static void to_uppercase(const char *src, char *dst, size_t size) {
  size_t i;
  for (i = 0; i < size - 1 && src[i]; i++) {
    if (src[i] >= 'a' && src[i] <= 'z') {
      dst[i] = src[i] - 32;
    } else {
      dst[i] = src[i];
    }
  }
  dst[i] = '\0';
}

static void update_detail_text(void) {
  WandbMetric *metric = get_current_metric();

  text_layer_set_text(s_detail.value_layer, metric->value);

  to_uppercase(metric->name, s_name_buffer, sizeof(s_name_buffer));
  text_layer_set_text(s_detail.name_layer, s_name_buffer);

  #if !defined(PBL_ROUND)
    WandbRun *run = &s_data.runs[s_ui.selected_run_index];
    snprintf(s_page_buffer, sizeof(s_page_buffer), "%d/%d",
             s_ui.current_metric_page + 1, run->num_metrics);
    text_layer_set_text(s_detail.pagination_layer, s_page_buffer);
  #endif

  s_ui.graph_display_page = s_ui.current_metric_page;
  mark_graph_dirty();
}

// Detail Window - Skeleton Loading State
static void skeleton_layer_update_proc(Layer *layer, GContext *ctx) {
  if (!s_ui.loading) return;

  graphics_context_set_fill_color(ctx, GColorLightGray);

  // Draw skeleton rectangle for name (slightly smaller than frame)
  GRect name_skeleton = s_detail.name_frame;
  name_skeleton.size.w = 80;
  name_skeleton.size.h = 14;
  name_skeleton.origin.y += 4;
  graphics_fill_rect(ctx, name_skeleton, 0, GCornerNone);

  // Draw skeleton rectangle for value (larger rectangle)
  GRect value_skeleton = s_detail.value_frame;
  value_skeleton.size.w = 100;
  value_skeleton.size.h = 26;
  value_skeleton.origin.y += 3;
  graphics_fill_rect(ctx, value_skeleton, 0, GCornerNone);

  // Draw skeleton rectangle for graph area
  graphics_fill_rect(ctx, s_detail.graph_frame, 0, GCornerNone);
}

// Detail Window - Graph Drawing
typedef struct {
  int64_t min;
  int64_t max;
  int64_t range;
} ValueRange;

static ValueRange calculate_value_range(const int64_t *values, uint8_t count) {
  ValueRange r = { .min = values[0], .max = values[0] };
  for (int i = 1; i < count; i++) {
    if (values[i] < r.min) r.min = values[i];
    if (values[i] > r.max) r.max = values[i];
  }
  r.range = (r.max - r.min) ? (r.max - r.min) : 1;
  return r;
}

static void calculate_graph_points(GPoint *points, const int64_t *history,
    uint8_t count, GRect bounds, ValueRange range) {
  int16_t graph_height = bounds.size.h - GRAPH_PADDING;
  int16_t graph_width = bounds.size.w - GRAPH_PADDING;

  for (int i = 0; i < count; i++) {
    int16_t x = GRAPH_MARGIN + (i * graph_width) / (count - 1);
    int16_t y = GRAPH_MARGIN + graph_height - ((history[i] - range.min) * graph_height / range.range);
    points[i] = GPoint(x, y);
  }
}

static void draw_data_points(GContext *ctx, const GPoint *points, uint8_t count) {
  #if defined(PBL_COLOR)
    graphics_context_set_fill_color(ctx, GColorLightGray);
  #else
    graphics_context_set_fill_color(ctx, GColorBlack);
  #endif
  for (int i = 0; i < count; i++) {
    graphics_fill_rect(ctx, GRect(points[i].x - 1, points[i].y - 1, DATA_POINT_SIZE, DATA_POINT_SIZE), 0, GCornerNone);
  }
}

static void draw_line_graph(GContext *ctx, const GPoint *points, uint8_t count) {
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, 2);
  for (int i = 1; i < count; i++) {
    graphics_draw_line(ctx, points[i - 1], points[i]);
  }
}

static GPoint interpolate_indicator_position(const GPoint *points, uint8_t count, int32_t index_fixed) {
  int idx_int = index_fixed / SCRUB_FIXED_SCALE;
  int32_t frac = index_fixed % SCRUB_FIXED_SCALE;

  // Clamp to valid range
  if (idx_int < 0) {
    idx_int = 0;
    frac = index_fixed;
  }
  if (idx_int >= count - 1) {
    idx_int = count - 2;
    frac = SCRUB_FIXED_SCALE + (index_fixed - (count - 1) * SCRUB_FIXED_SCALE);
  }

  // Interpolate between adjacent points
  int16_t x1 = points[idx_int].x, y1 = points[idx_int].y;
  int16_t x2 = points[idx_int + 1].x, y2 = points[idx_int + 1].y;

  return GPoint(
    x1 + (int16_t)((x2 - x1) * frac / SCRUB_FIXED_SCALE),
    y1 + (int16_t)((y2 - y1) * frac / SCRUB_FIXED_SCALE)
  );
}

static void draw_indicator(GContext *ctx, GPoint position) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  int16_t half = INDICATOR_SIZE / 2;
  graphics_fill_rect(ctx, GRect(position.x - half, position.y - half, INDICATOR_SIZE, INDICATOR_SIZE), 0, GCornerNone);
}

static void graph_layer_update_proc(Layer *layer, GContext *ctx) {
  if (s_ui.loading) return;

  GRect bounds = layer_get_bounds(layer);
  WandbMetric *metric = &s_data.runs[s_ui.selected_run_index].metrics[s_ui.graph_display_page];

  if (metric->history_count < 2) return;

  ValueRange range = calculate_value_range(metric->history, metric->history_count);

  GPoint points[MAX_HISTORY_POINTS];
  calculate_graph_points(points, metric->history, metric->history_count, bounds, range);

  if (s_scrub.active) {
    draw_data_points(ctx, points, metric->history_count);
  } else {
    draw_line_graph(ctx, points, metric->history_count);
  }

  GPoint indicator = s_scrub.active
    ? interpolate_indicator_position(points, metric->history_count, s_scrub.current_index_fixed)
    : points[metric->history_count - 1];

  draw_indicator(ctx, indicator);
}

// Detail Window - Animations
// Parse string to fixed-point integer (scaled by FIXED_POINT_SCALE)
static int32_t parse_fixed_point(const char *str, int *decimals) {
  int32_t result = 0;
  int sign = 1;
  int decimal_places = 0;
  bool seen_decimal = false;

  if (*str == '-') {
    sign = -1;
    str++;
  }

  while (*str) {
    if (*str == '.') {
      seen_decimal = true;
    } else if (*str >= '0' && *str <= '9') {
      result = result * 10 + (*str - '0');
      if (seen_decimal) {
        decimal_places++;
        if (decimal_places >= 4) break;  // Max 4 decimal places
      }
    } else {
      break;  // Stop on non-numeric character
    }
    str++;
  }

  // Scale to fixed point (4 decimal places)
  for (int i = decimal_places; i < 4; i++) {
    result *= 10;
  }

  *decimals = decimal_places;
  return sign * result;
}

// Format fixed-point value back to string
static void format_fixed_point(int64_t value, int decimals, char *buffer, size_t size) {
  bool negative = value < 0;
  if (negative) value = -value;

  int64_t integer_part = value / FIXED_POINT_SCALE;
  int64_t frac_part = value % FIXED_POINT_SCALE;

  if (decimals == 0) {
    snprintf(buffer, size, "%s%ld", negative ? "-" : "", (long)integer_part);
  } else {
    // Adjust fractional part based on desired decimals
    int divisor = 1;
    for (int i = 0; i < 4 - decimals; i++) divisor *= 10;
    frac_part /= divisor;

    char fmt[16];
    snprintf(fmt, sizeof(fmt), "%%s%%ld.%%0%dld", decimals);
    snprintf(buffer, size, fmt, negative ? "-" : "", (long)integer_part, (long)frac_part);
  }
}

// Custom update function for value interpolation
static void value_animation_update(Animation *animation, const AnimationProgress progress) {
  int32_t current = lerp_fixed(s_value_anim.from, s_value_anim.to, progress);
  format_fixed_point(current, s_value_anim.decimals, s_value_anim.buffer, sizeof(s_value_anim.buffer));
  text_layer_set_text(s_detail.value_layer, s_value_anim.buffer);
}

static void value_animation_teardown(Animation *animation) {
  WandbMetric *metric = get_current_metric();
  text_layer_set_text(s_detail.value_layer, metric->value);
}

static const AnimationImplementation s_value_animation_impl = {
  .update = value_animation_update,
  .teardown = value_animation_teardown,
};

static Animation *create_value_interpolation_animation(const char *from_value, const char *to_value) {
  int from_decimals, to_decimals;
  s_value_anim.from = parse_fixed_point(from_value, &from_decimals);
  s_value_anim.to = parse_fixed_point(to_value, &to_decimals);
  s_value_anim.decimals = (from_decimals > to_decimals) ? from_decimals : to_decimals;

  Animation *anim = animation_create();
  animation_set_implementation(anim, &s_value_animation_impl);
  animation_set_duration(anim, ANIM_DURATION);
  animation_set_curve(anim, AnimationCurveEaseInOut);
  return anim;
}

static void on_name_outbound_stopped(Animation *animation, bool finished, void *context) {
  WandbMetric *metric = get_current_metric();

  to_uppercase(metric->name, s_name_buffer, sizeof(s_name_buffer));
  text_layer_set_text(s_detail.name_layer, s_name_buffer);

  #if !defined(PBL_ROUND)
    WandbRun *run = &s_data.runs[s_ui.selected_run_index];
    snprintf(s_page_buffer, sizeof(s_page_buffer), "%d/%d",
             s_ui.current_metric_page + 1, run->num_metrics);
    text_layer_set_text(s_detail.pagination_layer, s_page_buffer);
  #endif

  s_ui.graph_display_page = s_ui.current_metric_page;
  mark_graph_dirty();
}

// Generic layer slide animation: out and back in from opposite direction
static Animation *create_layer_slide_animation(Layer *layer, GRect *home_frame,
    ScrollDirection direction, AnimationStoppedHandler on_halfway) {
  int16_t out_delta = (direction == ScrollDirectionUp) ? ANIM_SLIDE_DISTANCE : -ANIM_SLIDE_DISTANCE;

  GRect out_frame = *home_frame;
  out_frame.origin.y += out_delta;
  PropertyAnimation *anim_out = property_animation_create_layer_frame(layer, NULL, &out_frame);
  animation_set_duration((Animation *)anim_out, ANIM_DURATION / 2);
  animation_set_curve((Animation *)anim_out, AnimationCurveEaseIn);

  if (on_halfway) {
    animation_set_handlers((Animation *)anim_out, (AnimationHandlers) { .stopped = on_halfway }, NULL);
  }

  GRect in_frame = *home_frame;
  in_frame.origin.y -= out_delta;
  PropertyAnimation *anim_in = property_animation_create_layer_frame(layer, &in_frame, home_frame);
  animation_set_duration((Animation *)anim_in, ANIM_DURATION / 2);
  animation_set_curve((Animation *)anim_in, AnimationCurveEaseOut);

  return animation_sequence_create((Animation *)anim_out, (Animation *)anim_in, NULL);
}

// Generic layer bounce animation: out and back to same position
static Animation *create_layer_bounce_animation(Layer *layer, GRect *home_frame,
    ScrollDirection direction) {
  int16_t delta = (direction == ScrollDirectionUp) ? ANIM_SLIDE_DISTANCE / 3 : -ANIM_SLIDE_DISTANCE / 3;

  GRect bounce_frame = *home_frame;
  bounce_frame.origin.y += delta;
  PropertyAnimation *anim_out = property_animation_create_layer_frame(layer, NULL, &bounce_frame);
  animation_set_duration((Animation *)anim_out, ANIM_DURATION / 3);
  animation_set_curve((Animation *)anim_out, AnimationCurveEaseOut);

  PropertyAnimation *anim_back = property_animation_create_layer_frame(layer, &bounce_frame, home_frame);
  animation_set_duration((Animation *)anim_back, ANIM_DURATION / 3);
  animation_set_curve((Animation *)anim_back, AnimationCurveEaseIn);

  return animation_sequence_create((Animation *)anim_out, (Animation *)anim_back, NULL);
}

static Animation *create_scroll_animation(ScrollDirection direction, const char *old_value) {
  WandbMetric *metric = get_current_metric();

  Animation *value_anim = create_value_interpolation_animation(old_value, metric->value);
  Animation *name_anim = create_layer_slide_animation(
    text_layer_get_layer(s_detail.name_layer), &s_detail.name_frame, direction, on_name_outbound_stopped);
  Animation *graph_anim = create_layer_slide_animation(
    s_detail.graph_layer, &s_detail.graph_frame, direction, NULL);

  return animation_spawn_create(value_anim, name_anim, graph_anim, NULL);
}

static Animation *create_bounce_animation(ScrollDirection direction) {
  Animation *name_bounce = create_layer_bounce_animation(
    text_layer_get_layer(s_detail.name_layer), &s_detail.name_frame, direction);
  Animation *graph_bounce = create_layer_bounce_animation(
    s_detail.graph_layer, &s_detail.graph_frame, direction);

  return animation_spawn_create(name_bounce, graph_bounce, NULL);
}

static void do_scroll(ScrollDirection direction) {
  WandbRun *run = &s_data.runs[s_ui.selected_run_index];
  int delta = (direction == ScrollDirectionUp) ? -1 : 1;
  int next_page = s_ui.current_metric_page + delta;

  Animation *scroll_animation;

  if (next_page < 0 || next_page >= run->num_metrics) {
    scroll_animation = create_bounce_animation(direction);
  } else {
    const char *old_value = run->metrics[s_ui.current_metric_page].value;
    s_ui.current_metric_page = next_page;
    scroll_animation = create_scroll_animation(direction, old_value);
  }

  if (s_detail.scroll_animation) {
    animation_unschedule(s_detail.scroll_animation);
  }

  animation_schedule(scroll_animation);
  s_detail.scroll_animation = scroll_animation;
}

// Detail Window - Scrub Mode
// Forward declarations
static void do_scrub(int direction);
static void update_scrub_name_display(void);
static void stop_scrub_repeat(void);

static void update_scrub_value_display_interpolated(int32_t index_fixed) {
  WandbMetric *metric = get_current_metric();

  // Clamp index to valid range
  if (index_fixed < 0) index_fixed = 0;
  int32_t max_fixed = (metric->history_count - 1) * SCRUB_FIXED_SCALE;
  if (index_fixed > max_fixed) index_fixed = max_fixed;

  int idx_int = index_fixed / SCRUB_FIXED_SCALE;
  int32_t frac = index_fixed % SCRUB_FIXED_SCALE;

  // Interpolate between history values
  int64_t history_value;
  if (idx_int >= metric->history_count - 1) {
    history_value = metric->history[metric->history_count - 1];
  } else {
    int64_t v1 = metric->history[idx_int];
    int64_t v2 = metric->history[idx_int + 1];
    history_value = v1 + (v2 - v1) * frac / SCRUB_FIXED_SCALE;
  }

  int decimals;
  parse_fixed_point(metric->value, &decimals);
  format_fixed_point(history_value, decimals, s_scrub.value_buffer, sizeof(s_scrub.value_buffer));
  text_layer_set_text(s_detail.value_layer, s_scrub.value_buffer);
}

static void scrub_animation_update(Animation *animation, const AnimationProgress progress) {
  s_scrub.current_index_fixed = lerp_fixed(s_scrub.from_index_fixed, s_scrub.to_index_fixed, progress);
  update_scrub_value_display_interpolated(s_scrub.current_index_fixed);
  mark_graph_dirty();
}

static void scrub_animation_teardown(Animation *animation) {
  s_scrub.current_index_fixed = s_scrub.to_index_fixed;

  WandbMetric *metric = get_current_metric();
  int32_t max_fixed = (metric->history_count - 1) * SCRUB_FIXED_SCALE;

  if (s_scrub.current_index_fixed < 0) s_scrub.current_index_fixed = 0;
  if (s_scrub.current_index_fixed > max_fixed) s_scrub.current_index_fixed = max_fixed;

  s_scrub.index = s_scrub.current_index_fixed / SCRUB_FIXED_SCALE;
  update_scrub_value_display_interpolated(s_scrub.current_index_fixed);
  update_scrub_name_display();
  mark_graph_dirty();
  s_scrub.animation = NULL;
}

static const AnimationImplementation s_scrub_animation_impl = {
  .update = scrub_animation_update,
  .teardown = scrub_animation_teardown,
};

static void bounce_animation_update(Animation *animation, const AnimationProgress progress) {
  if (progress < ANIMATION_NORMALIZED_MAX / 2) {
    // First half: animate to bounce target
    AnimationProgress out_progress = progress * 2;
    s_scrub.current_index_fixed = lerp_fixed(s_scrub.from_index_fixed, s_scrub.bounce_target, out_progress);
  } else {
    // Second half: animate back to bounce return
    AnimationProgress back_progress = (progress - ANIMATION_NORMALIZED_MAX / 2) * 2;
    s_scrub.current_index_fixed = lerp_fixed(s_scrub.bounce_target, s_scrub.bounce_return, back_progress);
  }

  update_scrub_value_display_interpolated(s_scrub.current_index_fixed);
  mark_graph_dirty();
}

static void bounce_animation_teardown(Animation *animation) {
  s_scrub.current_index_fixed = s_scrub.bounce_return;
  update_scrub_value_display_interpolated(s_scrub.current_index_fixed);
  mark_graph_dirty();
  s_scrub.animation = NULL;
}

static const AnimationImplementation s_bounce_animation_impl = {
  .update = bounce_animation_update,
  .teardown = bounce_animation_teardown,
};

// Helper to schedule a scrub animation, canceling any existing one
static void schedule_scrub_animation(const AnimationImplementation *impl,
    uint32_t duration, AnimationCurve curve) {
  if (s_scrub.animation) {
    animation_unschedule(s_scrub.animation);
  }
  s_scrub.animation = animation_create();
  animation_set_implementation(s_scrub.animation, impl);
  animation_set_duration(s_scrub.animation, duration);
  animation_set_curve(s_scrub.animation, curve);
  animation_schedule(s_scrub.animation);
}

static void do_scrub(int direction) {
  WandbMetric *metric = get_current_metric();

  int32_t target_index = s_scrub.index + direction;
  int32_t max_index = metric->history_count - 1;

  // Complete any in-progress animation
  if (s_scrub.animation) {
    s_scrub.current_index_fixed = s_scrub.to_index_fixed;
  }

  if (target_index < 0 || target_index > max_index) {
    // Bounce at boundary
    int32_t bounce_amount = SCRUB_FIXED_SCALE / 3;
    s_scrub.from_index_fixed = s_scrub.current_index_fixed;
    s_scrub.bounce_target = (target_index < 0) ? -bounce_amount : max_index * SCRUB_FIXED_SCALE + bounce_amount;
    s_scrub.bounce_return = (target_index < 0) ? 0 : max_index * SCRUB_FIXED_SCALE;
    schedule_scrub_animation(&s_bounce_animation_impl, SCRUB_ANIM_DURATION, AnimationCurveEaseOut);
    return;
  }

  s_scrub.from_index_fixed = s_scrub.current_index_fixed;
  s_scrub.to_index_fixed = target_index * SCRUB_FIXED_SCALE;
  s_scrub.index = target_index;
  schedule_scrub_animation(&s_scrub_animation_impl, SCRUB_ANIM_DURATION, AnimationCurveEaseInOut);
}

static void wiggle_animation_update(Animation *animation, const AnimationProgress progress) {
  int32_t offset;
  if (progress < ANIMATION_NORMALIZED_MAX / 3) {
    AnimationProgress p = progress * 3;
    offset = -(int32_t)(((int64_t)p * s_scrub.wiggle_amount) / ANIMATION_NORMALIZED_MAX);
  } else if (progress < 2 * ANIMATION_NORMALIZED_MAX / 3) {
    AnimationProgress p = (progress - ANIMATION_NORMALIZED_MAX / 3) * 3;
    offset = -s_scrub.wiggle_amount + (int32_t)(((int64_t)p * s_scrub.wiggle_amount * 3 / 2) / ANIMATION_NORMALIZED_MAX);
  } else {
    AnimationProgress p = (progress - 2 * ANIMATION_NORMALIZED_MAX / 3) * 3;
    int32_t start_offset = s_scrub.wiggle_amount / 2;
    offset = start_offset - (int32_t)(((int64_t)p * start_offset) / ANIMATION_NORMALIZED_MAX);
  }

  s_scrub.current_index_fixed = s_scrub.wiggle_start + offset;
  mark_graph_dirty();
}

static void wiggle_animation_teardown(Animation *animation) {
  s_scrub.current_index_fixed = s_scrub.wiggle_start;
  update_scrub_value_display_interpolated(s_scrub.current_index_fixed);
  mark_graph_dirty();
  s_scrub.animation = NULL;
}

static const AnimationImplementation s_wiggle_animation_impl = {
  .update = wiggle_animation_update,
  .teardown = wiggle_animation_teardown,
};

static void update_scrub_name_display(void) {
  WandbMetric *metric = get_current_metric();

  to_uppercase(metric->name, s_name_buffer, sizeof(s_name_buffer));
  if (s_scrub.active && s_scrub.index < metric->history_count - 1) {
    strncat(s_name_buffer, " (-)", sizeof(s_name_buffer) - strlen(s_name_buffer) - 1);
  }
  text_layer_set_text(s_detail.name_layer, s_name_buffer);
}

static void enter_scrub_mode(void) {
  WandbMetric *metric = get_current_metric();

  s_scrub.active = true;
  s_scrub.index = metric->history_count - 1;
  s_scrub.current_index_fixed = s_scrub.index * SCRUB_FIXED_SCALE;

  update_scrub_value_display_interpolated(s_scrub.current_index_fixed);
  update_scrub_name_display();
  mark_graph_dirty();

  s_scrub.wiggle_start = s_scrub.current_index_fixed;
  s_scrub.wiggle_amount = SCRUB_FIXED_SCALE;
  schedule_scrub_animation(&s_wiggle_animation_impl, WIGGLE_ANIM_DURATION, AnimationCurveLinear);
}

static void exit_scrub_animation_teardown(Animation *animation) {
  s_scrub.active = false;
  s_scrub.animation = NULL;

  WandbMetric *metric = get_current_metric();
  text_layer_set_text(s_detail.value_layer, metric->value);

  to_uppercase(metric->name, s_name_buffer, sizeof(s_name_buffer));
  text_layer_set_text(s_detail.name_layer, s_name_buffer);

  mark_graph_dirty();
}

static const AnimationImplementation s_exit_scrub_animation_impl = {
  .update = scrub_animation_update,
  .teardown = exit_scrub_animation_teardown,
};

static void exit_scrub_mode(void) {
  stop_scrub_repeat();

  WandbMetric *metric = get_current_metric();
  s_scrub.from_index_fixed = s_scrub.current_index_fixed;
  s_scrub.to_index_fixed = (metric->history_count - 1) * SCRUB_FIXED_SCALE;
  schedule_scrub_animation(&s_exit_scrub_animation_impl, SCRUB_ANIM_DURATION * 2, AnimationCurveEaseOut);
}

// Detail Window - Click Handlers
static void scrub_repeat_timer_callback(void *data) {
  if (s_scrub.active && s_scrub.repeat_direction != 0) {
    do_scrub(s_scrub.repeat_direction);
    s_scrub.repeat_timer = app_timer_register(SCRUB_REPEAT_INTERVAL, scrub_repeat_timer_callback, NULL);
  } else {
    s_scrub.repeat_timer = NULL;
  }
}

static void stop_scrub_repeat(void) {
  if (s_scrub.repeat_timer) {
    app_timer_cancel(s_scrub.repeat_timer);
    s_scrub.repeat_timer = NULL;
  }
  s_scrub.repeat_direction = 0;
}

static void detail_up_down_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_ui.loading) return;

  ButtonId button = click_recognizer_get_button_id(recognizer);
  int direction = (button == BUTTON_ID_UP) ? 1 : -1;

  if (s_scrub.active) {
    if (s_scrub.repeat_timer) {
      app_timer_cancel(s_scrub.repeat_timer);
    }
    do_scrub(direction);
    s_scrub.repeat_direction = direction;
    s_scrub.repeat_timer = app_timer_register(SCRUB_REPEAT_INTERVAL, scrub_repeat_timer_callback, NULL);
  } else {
    do_scroll((button == BUTTON_ID_UP) ? ScrollDirectionUp : ScrollDirectionDown);
  }
}

static void detail_up_down_release_handler(ClickRecognizerRef recognizer, void *context) {
  stop_scrub_repeat();
}

static void detail_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_ui.loading) return;

  if (s_scrub.active) {
    exit_scrub_mode();
  } else {
    enter_scrub_mode();
  }
}

static void detail_click_config_provider(void *context) {
  window_raw_click_subscribe(BUTTON_ID_UP, detail_up_down_handler, detail_up_down_release_handler, NULL);
  window_raw_click_subscribe(BUTTON_ID_DOWN, detail_up_down_handler, detail_up_down_release_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_SELECT, detail_select_click_handler);
}

// Detail Window - Lifecycle
// Helper to create a styled status bar with dotted separator
static StatusBarLayer* create_status_bar(Layer *parent) {
  StatusBarLayer *status_bar = status_bar_layer_create();
  status_bar_layer_set_colors(status_bar, GColorClear, GColorBlack);
  status_bar_layer_set_separator_mode(status_bar, StatusBarLayerSeparatorModeDotted);
  layer_add_child(parent, status_bar_layer_get_layer(status_bar));
  return status_bar;
}

static void detail_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_detail.status_bar = create_status_bar(window_layer);

  // Platform-specific layout configuration
  #if defined(PBL_ROUND)
    const int16_t padding = 30;
    const int16_t name_y = STATUS_BAR_HEIGHT + padding / 2;
    const int16_t content_width = bounds.size.w - padding * 2;
    const GTextAlignment text_align = GTextAlignmentCenter;
    const int16_t graph_inset = 10;
  #else
    const int16_t padding = PADDING_LEFT;
    const int16_t name_y = STATUS_BAR_HEIGHT + padding;
    const int16_t content_width = bounds.size.w - padding * 2;
    const GTextAlignment text_align = GTextAlignmentLeft;
    const int16_t graph_inset = 0;

    // Create pagination layer (rectangular displays only)
    #if defined(PBL_PLATFORM_EMERY)
      s_detail.pagination_layer = text_layer_create(GRect(bounds.size.w - 50, -2, 46, 22));
      text_layer_set_font(s_detail.pagination_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
    #else
      s_detail.pagination_layer = text_layer_create(GRect(bounds.size.w - 40, -2, 36, STATUS_BAR_HEIGHT));
      text_layer_set_font(s_detail.pagination_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
    #endif
    text_layer_set_text_alignment(s_detail.pagination_layer, GTextAlignmentRight);
    text_layer_set_background_color(s_detail.pagination_layer, GColorClear);
    layer_add_child(window_layer, text_layer_get_layer(s_detail.pagination_layer));
  #endif

  const int16_t value_y = name_y + 22;
  const int16_t graph_y = value_y + 32 + padding;
  const int16_t graph_height = bounds.size.h - graph_y - padding;

  // Store frames for animation
  s_detail.name_frame = GRect(padding, name_y, content_width, 22);
  s_detail.value_frame = GRect(padding, value_y, content_width, 32);
  s_detail.graph_frame = GRect(padding + graph_inset, graph_y, content_width - graph_inset * 2 - padding, graph_height);

  // Name layer
  s_detail.name_layer = text_layer_create(s_detail.name_frame);
  text_layer_set_font(s_detail.name_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_detail.name_layer, text_align);
  text_layer_set_background_color(s_detail.name_layer, GColorClear);
  layer_add_child(window_layer, text_layer_get_layer(s_detail.name_layer));

  // Value layer
  s_detail.value_layer = text_layer_create(s_detail.value_frame);
  text_layer_set_font(s_detail.value_layer, fonts_get_system_font(FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM));
  text_layer_set_text_alignment(s_detail.value_layer, text_align);
  text_layer_set_background_color(s_detail.value_layer, GColorClear);
  layer_add_child(window_layer, text_layer_get_layer(s_detail.value_layer));

  // Graph layer
  s_detail.graph_layer = layer_create(s_detail.graph_frame);
  layer_set_update_proc(s_detail.graph_layer, graph_layer_update_proc);
  layer_set_clips(s_detail.graph_layer, false);
  layer_add_child(window_layer, s_detail.graph_layer);

  // Skeleton layer (drawn on top when loading)
  s_detail.skeleton_layer = layer_create(bounds);
  layer_set_update_proc(s_detail.skeleton_layer, skeleton_layer_update_proc);
  layer_add_child(window_layer, s_detail.skeleton_layer);

  // Reset state
  s_detail.scroll_animation = NULL;
  s_scrub.active = false;

  // Don't call update_detail_text() when loading - skeleton shows instead
  if (!s_ui.loading) {
    update_detail_text();
  }
}

static void detail_window_unload(Window *window) {
  s_scrub.active = false;
  s_ui.loading = false;

  if (s_detail.loading_timer) {
    app_timer_cancel(s_detail.loading_timer);
    s_detail.loading_timer = NULL;
  }
  if (s_detail.scroll_animation) {
    animation_unschedule(s_detail.scroll_animation);
    s_detail.scroll_animation = NULL;
  }
  text_layer_destroy(s_detail.value_layer);
  text_layer_destroy(s_detail.name_layer);
  layer_destroy(s_detail.graph_layer);
  layer_destroy(s_detail.skeleton_layer);
  #if !defined(PBL_ROUND)
    text_layer_destroy(s_detail.pagination_layer);
  #endif
  status_bar_layer_destroy(s_detail.status_bar);
  window_destroy(s_detail.window);
  s_detail.window = NULL;
}

static void hide_detail_loading(void) {
  s_ui.loading = false;
  if (s_detail.loading_timer) {
    app_timer_cancel(s_detail.loading_timer);
    s_detail.loading_timer = NULL;
  }
  update_detail_text();
  layer_mark_dirty(s_detail.skeleton_layer);
}

static void detail_loading_timer_callback(void *data) {
  s_detail.loading_timer = NULL;
  if (!s_ui.loading) return;

  // Timeout - show error and hide skeleton
  s_ui.loading = false;
  text_layer_set_text(s_detail.value_layer, "Error");
  text_layer_set_text(s_detail.name_layer, "NO METRICS");
  layer_mark_dirty(s_detail.skeleton_layer);
}

static void detail_window_push(void) {
  s_ui.loading = true;

  s_detail.window = window_create();
  window_set_click_config_provider(s_detail.window, detail_click_config_provider);
  window_set_window_handlers(s_detail.window, (WindowHandlers) {
    .load = detail_window_load,
    .unload = detail_window_unload,
  });
  window_stack_push(s_detail.window, true);

  // Schedule loading timeout (8 seconds)
  s_detail.loading_timer = app_timer_register(8000, detail_loading_timer_callback, NULL);
}

// Main Menu Window
static char s_header_buffer[MAX_STATE_LENGTH];

static void to_uppercase_state(const char *src, char *dst, size_t size) {
  size_t i;
  for (i = 0; i < size - 1 && src[i]; i++) {
    dst[i] = (src[i] >= 'a' && src[i] <= 'z') ? src[i] - 32 : src[i];
  }
  dst[i] = '\0';
}

static uint16_t menu_get_num_sections_callback(MenuLayer *menu_layer, void *data) {
  return count_unique_states();
}

static uint16_t menu_get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
  const char *state = get_state_for_section(section_index);
  return state ? count_runs_with_state(state) : 0;
}

static int16_t menu_get_header_height_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
  const char *state = get_state_for_section(section_index);
  return state ? 18 : 0;
}

static void menu_draw_header_callback(GContext *ctx, const Layer *cell_layer, uint16_t section_index, void *data) {
  const char *state = get_state_for_section(section_index);
  if (!state) return;

  to_uppercase_state(state, s_header_buffer, sizeof(s_header_buffer));

  GRect bounds = layer_get_bounds(cell_layer);
  GRect text_bounds = GRect(4, 0, bounds.size.w - 8, bounds.size.h);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, s_header_buffer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     text_bounds, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void menu_draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
  int8_t run_index = get_run_index_for_section_row(cell_index->section, cell_index->row);
  if (run_index < 0) return;
  WandbRun *run = &s_data.runs[run_index];
  menu_cell_basic_draw(ctx, cell_layer, run->run_name, run->project_name, NULL);
}

static void request_metrics_for_run(uint8_t run_index) {
  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);
  if (result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to begin outbox: %d", result);
    return;
  }

  dict_write_uint8(iter, MESSAGE_KEY_FETCH_RUN_INDEX, run_index);
  result = app_message_outbox_send();
  if (result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to send outbox: %d", result);
  }
}

static void menu_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  int8_t run_index = get_run_index_for_section_row(cell_index->section, cell_index->row);
  if (run_index < 0) return;
  s_ui.selected_run_index = run_index;
  s_ui.current_metric_page = 0;

  // Request metrics from JS
  request_metrics_for_run(run_index);

  detail_window_push();
}

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  GRect menu_bounds = GRect(0, STATUS_BAR_HEIGHT, bounds.size.w, bounds.size.h - STATUS_BAR_HEIGHT);
  s_main.menu = menu_layer_create(menu_bounds);

  menu_layer_set_callbacks(s_main.menu, NULL, (MenuLayerCallbacks) {
    .get_num_sections = menu_get_num_sections_callback,
    .get_num_rows = menu_get_num_rows_callback,
    .get_header_height = menu_get_header_height_callback,
    .draw_header = menu_draw_header_callback,
    .draw_row = menu_draw_row_callback,
    .select_click = menu_select_callback,
  });

  menu_layer_set_click_config_onto_window(s_main.menu, window);

  #if defined(PBL_COLOR)
    menu_layer_set_normal_colors(s_main.menu, GColorWhite, GColorBlack);
    menu_layer_set_highlight_colors(s_main.menu, GColorBlack, GColorWhite);
  #endif

  layer_add_child(window_layer, menu_layer_get_layer(s_main.menu));

  // Loading text layer - centered horizontally and vertically
  int16_t content_height = bounds.size.h - STATUS_BAR_HEIGHT;
  int16_t text_height = 96;  // Allow for 3 lines
  int16_t loading_y = STATUS_BAR_HEIGHT + (content_height - text_height) / 2;
  GRect loading_bounds = GRect(PADDING_LEFT, loading_y, bounds.size.w - PADDING_LEFT * 2, text_height);
  s_main.loading_layer = text_layer_create(loading_bounds);
  text_layer_set_font(s_main.loading_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text(s_main.loading_layer, "Talking with Weights & Biases...");
  text_layer_set_text_alignment(s_main.loading_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_main.loading_layer, GColorWhite);
  layer_add_child(window_layer, text_layer_get_layer(s_main.loading_layer));

  // Show/hide layers based on loading state
  layer_set_hidden(menu_layer_get_layer(s_main.menu), s_main.loading);
  layer_set_hidden(text_layer_get_layer(s_main.loading_layer), !s_main.loading);

  s_main.status_bar = create_status_bar(window_layer);
}

static void main_window_unload(Window *window) {
  if (s_main.loading_timer) {
    app_timer_cancel(s_main.loading_timer);
    s_main.loading_timer = NULL;
  }
  menu_layer_destroy(s_main.menu);
  text_layer_destroy(s_main.loading_layer);
  status_bar_layer_destroy(s_main.status_bar);
}

static void hide_main_loading(void) {
  s_main.loading = false;
  if (s_main.loading_timer) {
    app_timer_cancel(s_main.loading_timer);
    s_main.loading_timer = NULL;
  }
  layer_set_hidden(menu_layer_get_layer(s_main.menu), false);
  layer_set_hidden(text_layer_get_layer(s_main.loading_layer), true);
  menu_layer_reload_data(s_main.menu);
}

static void main_loading_timer_callback(void *data) {
  s_main.loading_timer = NULL;
  if (!s_main.loading) return;

  // Timeout - show "No runs" message
  text_layer_set_text(s_main.loading_layer, "Could not load runs. Check your API key.");
}

// AppMessage Handling
static void inbox_received_callback(DictionaryIterator *iter, void *context) {
  // Check for RUNS_COUNT (sent with first message)
  Tuple *count_tuple = dict_find(iter, MESSAGE_KEY_RUNS_COUNT);
  if (count_tuple) {
    s_expected_runs_count = count_tuple->value->uint8;
    s_data.num_runs = 0;  // Reset for new data

    // Handle 0 runs case immediately
    if (s_expected_runs_count == 0) {
      hide_main_loading();
      return;
    }
  }

  // Get run data
  Tuple *name_tuple = dict_find(iter, MESSAGE_KEY_RUN_NAME);
  Tuple *source_tuple = dict_find(iter, MESSAGE_KEY_RUN_OWNER);
  Tuple *state_tuple = dict_find(iter, MESSAGE_KEY_RUN_STATE);

  if (name_tuple && source_tuple && state_tuple && s_data.num_runs < MAX_RUNS) {
    WandbRun *run = &s_data.runs[s_data.num_runs];

    strncpy(run->run_name, name_tuple->value->cstring, MAX_NAME_LENGTH - 1);
    run->run_name[MAX_NAME_LENGTH - 1] = '\0';

    strncpy(run->project_name, source_tuple->value->cstring, MAX_NAME_LENGTH - 1);
    run->project_name[MAX_NAME_LENGTH - 1] = '\0';

    strncpy(run->state, state_tuple->value->cstring, MAX_STATE_LENGTH - 1);
    run->state[MAX_STATE_LENGTH - 1] = '\0';
    run->num_metrics = 0;

    s_data.num_runs++;

    // Check if all runs received
    if (s_data.num_runs >= s_expected_runs_count) {
      hide_main_loading();
    }
  }

  // Check for METRICS_COUNT (sent with first metric message)
  Tuple *metrics_count_tuple = dict_find(iter, MESSAGE_KEY_METRICS_COUNT);
  if (metrics_count_tuple) {
    s_expected_metrics_count = metrics_count_tuple->value->uint8;
    // Reset metrics for current run
    WandbRun *run = &s_data.runs[s_ui.selected_run_index];
    run->num_metrics = 0;

    // Handle 0 metrics case immediately
    if (s_expected_metrics_count == 0) {
      hide_detail_loading();
      return;
    }
  }

  // Get metric data
  Tuple *metric_name_tuple = dict_find(iter, MESSAGE_KEY_METRIC_NAME);
  Tuple *metric_value_tuple = dict_find(iter, MESSAGE_KEY_METRIC_VALUE);
  Tuple *metric_history_tuple = dict_find(iter, MESSAGE_KEY_METRIC_HISTORY);

  if (metric_name_tuple && metric_value_tuple) {
    WandbRun *run = &s_data.runs[s_ui.selected_run_index];

    // Only accept if we have room
    if (run->num_metrics < MAX_METRICS_PER_RUN) {
      WandbMetric *metric = &run->metrics[run->num_metrics];

      strncpy(metric->name, metric_name_tuple->value->cstring, MAX_NAME_LENGTH - 1);
      metric->name[MAX_NAME_LENGTH - 1] = '\0';

      strncpy(metric->value, metric_value_tuple->value->cstring, MAX_VALUE_LENGTH - 1);
      metric->value[MAX_VALUE_LENGTH - 1] = '\0';

      // Parse history if present (packed int64 array)
      metric->history_count = 0;
      if (metric_history_tuple && metric_history_tuple->length > 0) {
        uint8_t *bytes = metric_history_tuple->value->data;
        uint16_t num_points = metric_history_tuple->length / 8;
        if (num_points > MAX_HISTORY_POINTS) num_points = MAX_HISTORY_POINTS;

        for (int i = 0; i < num_points; i++) {
          // Little-endian int64
          uint32_t low = bytes[i * 8] |
            (bytes[i * 8 + 1] << 8) |
            (bytes[i * 8 + 2] << 16) |
            (bytes[i * 8 + 3] << 24);
          uint32_t high = bytes[i * 8 + 4] |
            (bytes[i * 8 + 5] << 8) |
            (bytes[i * 8 + 6] << 16) |
            (bytes[i * 8 + 7] << 24);
          metric->history[i] = (int64_t)(((uint64_t)high << 32) | low);
        }
        metric->history_count = num_points;
      }

      run->num_metrics++;
    }

    // Check if all metrics received (or we're full)
    if (run->num_metrics >= s_expected_metrics_count || run->num_metrics >= MAX_METRICS_PER_RUN) {
      hide_detail_loading();
    }
  }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped: %d", reason);
}

// App Lifecycle
static void prv_init(void) {
  // Initialize AppMessage
  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_open(512, 64);

  s_main.loading = true;
  s_main.window = window_create();
  window_set_window_handlers(s_main.window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_main.window, true);

  // Schedule loading timeout (8 seconds)
  s_main.loading_timer = app_timer_register(8000, main_loading_timer_callback, NULL);
}

static void prv_deinit(void) {
  window_destroy(s_main.window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
