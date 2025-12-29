#include <pebble.h>

//==============================================================================
// Constants and Data Structures
//==============================================================================

#define MAX_RUNS 10
#define MAX_METRICS_PER_RUN 8
#define MAX_NAME_LENGTH 32
#define MAX_VALUE_LENGTH 16
#define MAX_HISTORY_POINTS 20

#define PADDING_LEFT 10
#define ANIM_DURATION 200
#define ANIM_SLIDE_DISTANCE 15
#define STATUS_BAR_HEIGHT 16

typedef enum {
  ScrollDirectionUp,
  ScrollDirectionDown,
} ScrollDirection;

typedef struct {
  char name[MAX_NAME_LENGTH];
  char value[MAX_VALUE_LENGTH];
  int32_t history[MAX_HISTORY_POINTS];  // Fixed-point historical values
  uint8_t history_count;
} WandbMetric;

typedef struct {
  char run_name[MAX_NAME_LENGTH];
  char project_name[MAX_NAME_LENGTH];
  WandbMetric metrics[MAX_METRICS_PER_RUN];
  uint8_t num_metrics;
} WandbRun;

typedef struct {
  WandbRun runs[MAX_RUNS];
  uint8_t num_runs;
  uint8_t selected_run_index;
  uint8_t current_metric_page;
} AppData;

//==============================================================================
// Static Variables
//==============================================================================

static Window *s_main_window;
static MenuLayer *s_menu_layer;
static StatusBarLayer *s_main_status_bar;

static Window *s_detail_window;
static TextLayer *s_value_layer;
static TextLayer *s_name_layer;
static Layer *s_graph_layer;
static StatusBarLayer *s_detail_status_bar;
static TextLayer *s_pagination_layer;

static AppData s_app_data;
static char s_page_buffer[16];
static char s_name_buffer[MAX_NAME_LENGTH];

// Animation state
static Animation *s_current_animation;
static GRect s_value_layer_frame;
static GRect s_name_layer_frame;
static GRect s_graph_layer_frame;
static uint8_t s_graph_display_page;  // Which page the graph should currently draw

// Scrub mode state
static bool s_scrub_mode = false;
static uint8_t s_scrub_index = 0;
static char s_scrub_value_buffer[MAX_VALUE_LENGTH];
static Layer *s_action_button_layer;

// Scrub animation state
static Animation *s_scrub_animation = NULL;
static int32_t s_scrub_from_index_fixed;  // Fixed-point (scaled by 1000)
static int32_t s_scrub_to_index_fixed;
static int32_t s_scrub_current_index_fixed;  // Current animated position
#define SCRUB_FIXED_SCALE 1000
#define SCRUB_ANIM_DURATION 100

// Scrub repeat timer state
static AppTimer *s_scrub_repeat_timer = NULL;
static int s_scrub_repeat_direction = 0;
#define SCRUB_REPEAT_INTERVAL 150

// Value interpolation state (fixed-point with 4 decimal places)
#define FIXED_POINT_SCALE 10000
static int32_t s_value_from;
static int32_t s_value_to;
static int s_value_decimals;  // Number of decimal places to show
static char s_animated_value_buffer[MAX_VALUE_LENGTH];

//==============================================================================
// Mock Data Initialization
//==============================================================================

// Helper to set mock history data
static void set_metric_history(WandbMetric *metric, int32_t *values, uint8_t count) {
  metric->history_count = count;
  for (int i = 0; i < count; i++) {
    metric->history[i] = values[i];
  }
}

static void init_mock_data(void) {
  s_app_data.num_runs = 3;

  // All history values must be in FIXED_POINT_SCALE (10000) format
  // "0.9523" → 9523, "50" → 500000, "12.45" → 124500

  // Run 1: sunny-moon-42 / image-classifier
  strncpy(s_app_data.runs[0].run_name, "sunny-moon-42", MAX_NAME_LENGTH);
  strncpy(s_app_data.runs[0].project_name, "image-classifier", MAX_NAME_LENGTH);
  s_app_data.runs[0].num_metrics = 4;

  // accuracy = "0.9523" → 9523 (4 decimals, already at scale)
  strncpy(s_app_data.runs[0].metrics[0].name, "accuracy", MAX_NAME_LENGTH);
  strncpy(s_app_data.runs[0].metrics[0].value, "0.9523", MAX_VALUE_LENGTH);
  int32_t acc_hist[] = {5000, 6500, 7200, 7800, 8200, 8500, 8800, 9000, 9200, 9350, 9450, 9523};
  set_metric_history(&s_app_data.runs[0].metrics[0], acc_hist, 12);

  // loss = "0.0234" → 234 (4 decimals, already at scale)
  strncpy(s_app_data.runs[0].metrics[1].name, "loss", MAX_NAME_LENGTH);
  strncpy(s_app_data.runs[0].metrics[1].value, "0.0234", MAX_VALUE_LENGTH);
  int32_t loss_hist[] = {8500, 6200, 4500, 3200, 2100, 1400, 900, 600, 400, 300, 250, 234};
  set_metric_history(&s_app_data.runs[0].metrics[1], loss_hist, 12);

  // epoch = "50" → 500000 (0 decimals, scale by 10000)
  strncpy(s_app_data.runs[0].metrics[2].name, "epoch", MAX_NAME_LENGTH);
  strncpy(s_app_data.runs[0].metrics[2].value, "50", MAX_VALUE_LENGTH);
  int32_t epoch_hist[] = {0, 50000, 100000, 150000, 200000, 250000, 300000, 350000, 400000, 450000, 500000};
  set_metric_history(&s_app_data.runs[0].metrics[2], epoch_hist, 11);

  // lr = "0.0001" → 1 (4 decimals, already at scale)
  strncpy(s_app_data.runs[0].metrics[3].name, "lr", MAX_NAME_LENGTH);
  strncpy(s_app_data.runs[0].metrics[3].value, "0.0001", MAX_VALUE_LENGTH);
  int32_t lr_hist[] = {10, 10, 10, 5, 5, 5, 2, 2, 1, 1};
  set_metric_history(&s_app_data.runs[0].metrics[3], lr_hist, 10);

  // Run 2: cosmic-river-7 / text-gen
  strncpy(s_app_data.runs[1].run_name, "cosmic-river-7", MAX_NAME_LENGTH);
  strncpy(s_app_data.runs[1].project_name, "text-gen", MAX_NAME_LENGTH);
  s_app_data.runs[1].num_metrics = 4;

  // perplexity = "12.45" → 124500 (2 decimals, scale by 100)
  strncpy(s_app_data.runs[1].metrics[0].name, "perplexity", MAX_NAME_LENGTH);
  strncpy(s_app_data.runs[1].metrics[0].value, "12.45", MAX_VALUE_LENGTH);
  int32_t ppl_hist[] = {4500000, 3200000, 2200000, 1500000, 1000000, 700000, 500000, 350000, 250000, 180000, 150000, 124500};
  set_metric_history(&s_app_data.runs[1].metrics[0], ppl_hist, 12);

  // tokens = "120" → 1200000 (smaller value to avoid overflow)
  strncpy(s_app_data.runs[1].metrics[1].name, "tokens", MAX_NAME_LENGTH);
  strncpy(s_app_data.runs[1].metrics[1].value, "120", MAX_VALUE_LENGTH);
  int32_t tok_hist[] = {100000, 200000, 350000, 500000, 650000, 800000, 900000, 1000000, 1100000, 1200000};
  set_metric_history(&s_app_data.runs[1].metrics[1], tok_hist, 10);

  // steps = "50" → 500000
  strncpy(s_app_data.runs[1].metrics[2].name, "steps", MAX_NAME_LENGTH);
  strncpy(s_app_data.runs[1].metrics[2].value, "50", MAX_VALUE_LENGTH);
  int32_t steps_hist[] = {0, 50000, 100000, 150000, 200000, 250000, 300000, 350000, 400000, 450000, 500000};
  set_metric_history(&s_app_data.runs[1].metrics[2], steps_hist, 11);

  // loss = "2.341" → 23410 (3 decimals, scale by 10)
  strncpy(s_app_data.runs[1].metrics[3].name, "loss", MAX_NAME_LENGTH);
  strncpy(s_app_data.runs[1].metrics[3].value, "2.341", MAX_VALUE_LENGTH);
  int32_t loss2_hist[] = {95000, 72000, 58000, 45000, 38000, 32000, 29000, 26500, 25000, 24000, 23410};
  set_metric_history(&s_app_data.runs[1].metrics[3], loss2_hist, 11);

  // Run 3: rapid-forest-99 / rl-agent
  strncpy(s_app_data.runs[2].run_name, "rapid-forest-99", MAX_NAME_LENGTH);
  strncpy(s_app_data.runs[2].project_name, "rl-agent", MAX_NAME_LENGTH);
  s_app_data.runs[2].num_metrics = 4;

  // reward = "48.72" → 487200 (2 decimals, scale by 100)
  strncpy(s_app_data.runs[2].metrics[0].name, "reward", MAX_NAME_LENGTH);
  strncpy(s_app_data.runs[2].metrics[0].value, "48.72", MAX_VALUE_LENGTH);
  int32_t reward_hist[] = {-50000, 20000, 80000, 150000, 200000, 250000, 300000, 350000, 400000, 430000, 460000, 487200};
  set_metric_history(&s_app_data.runs[2].metrics[0], reward_hist, 12);

  // episodes = "100" → 1000000
  strncpy(s_app_data.runs[2].metrics[1].name, "episodes", MAX_NAME_LENGTH);
  strncpy(s_app_data.runs[2].metrics[1].value, "100", MAX_VALUE_LENGTH);
  int32_t ep_hist[] = {0, 100000, 200000, 300000, 400000, 500000, 600000, 700000, 800000, 900000, 1000000};
  set_metric_history(&s_app_data.runs[2].metrics[1], ep_hist, 11);

  // epsilon = "0.05" → 500 (2 decimals, scale by 100)
  strncpy(s_app_data.runs[2].metrics[2].name, "epsilon", MAX_NAME_LENGTH);
  strncpy(s_app_data.runs[2].metrics[2].value, "0.05", MAX_VALUE_LENGTH);
  int32_t eps_hist[] = {10000, 9000, 8000, 7000, 5000, 3500, 2000, 1500, 1000, 700, 500};
  set_metric_history(&s_app_data.runs[2].metrics[2], eps_hist, 11);

  // score = "98" → 980000 (smaller value to avoid large numbers)
  strncpy(s_app_data.runs[2].metrics[3].name, "score", MAX_NAME_LENGTH);
  strncpy(s_app_data.runs[2].metrics[3].value, "98", MAX_VALUE_LENGTH);
  int32_t score_hist[] = {10000, 50000, 120000, 250000, 400000, 550000, 680000, 780000, 860000, 930000, 980000};
  set_metric_history(&s_app_data.runs[2].metrics[3], score_hist, 11);
}

//==============================================================================
// Detail Window - Display Updates
//==============================================================================

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
  WandbRun *run = &s_app_data.runs[s_app_data.selected_run_index];
  WandbMetric *metric = &run->metrics[s_app_data.current_metric_page];

  text_layer_set_text(s_value_layer, metric->value);

  to_uppercase(metric->name, s_name_buffer, sizeof(s_name_buffer));
  text_layer_set_text(s_name_layer, s_name_buffer);

  #if !defined(PBL_ROUND)
    snprintf(s_page_buffer, sizeof(s_page_buffer), "%d/%d",
             s_app_data.current_metric_page + 1,
             run->num_metrics);
    text_layer_set_text(s_pagination_layer, s_page_buffer);
  #endif

  // Set which page the graph should display and trigger redraw
  s_graph_display_page = s_app_data.current_metric_page;
  if (s_graph_layer) {
    layer_mark_dirty(s_graph_layer);
  }
}

//==============================================================================
// Detail Window - Graph Drawing
//==============================================================================

static void graph_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  WandbRun *run = &s_app_data.runs[s_app_data.selected_run_index];
  WandbMetric *metric = &run->metrics[s_graph_display_page];

  if (metric->history_count < 2) return;

  // Find min/max values for scaling
  int32_t min_val = metric->history[0];
  int32_t max_val = metric->history[0];
  for (int i = 1; i < metric->history_count; i++) {
    if (metric->history[i] < min_val) min_val = metric->history[i];
    if (metric->history[i] > max_val) max_val = metric->history[i];
  }

  // Add some padding to the range
  int32_t range = max_val - min_val;
  if (range == 0) range = 1;  // Prevent division by zero

  int16_t graph_height = bounds.size.h - 4;  // Leave margin
  int16_t graph_width = bounds.size.w - 4;
  int16_t x_offset = 2;
  int16_t y_offset = 2;

  // Store all points for interpolation
  GPoint points[MAX_HISTORY_POINTS];

  for (int i = 0; i < metric->history_count; i++) {
    int16_t x = x_offset + (i * graph_width) / (metric->history_count - 1);
    int16_t y = y_offset + graph_height - ((metric->history[i] - min_val) * graph_height / range);
    points[i] = GPoint(x, y);
  }

  if (s_scrub_mode) {
    // In scrub mode, draw small squares at each data point
    #if defined(PBL_COLOR)
      graphics_context_set_fill_color(ctx, GColorLightGray);
    #else
      graphics_context_set_fill_color(ctx, GColorBlack);
    #endif
    for (int i = 0; i < metric->history_count; i++) {
      graphics_fill_rect(ctx, GRect(points[i].x - 1, points[i].y - 1, 3, 3), 0, GCornerNone);
    }
  } else {
    // Normal mode: draw connected line graph
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_stroke_width(ctx, 2);
    for (int i = 1; i < metric->history_count; i++) {
      graphics_draw_line(ctx, points[i - 1], points[i]);
    }
  }

  // Calculate indicator position
  GPoint indicator_point;
  if (s_scrub_mode) {
    // Use animated position (interpolate between points)
    int32_t index_fixed = s_scrub_current_index_fixed;
    int idx_int = index_fixed / SCRUB_FIXED_SCALE;
    int32_t frac = index_fixed % SCRUB_FIXED_SCALE;

    // Clamp to valid range
    if (idx_int < 0) {
      idx_int = 0;
      frac = index_fixed;  // Can be negative for bounce
    }
    if (idx_int >= metric->history_count - 1) {
      idx_int = metric->history_count - 2;
      frac = SCRUB_FIXED_SCALE + (index_fixed - (metric->history_count - 1) * SCRUB_FIXED_SCALE);
    }

    // Interpolate between points[idx_int] and points[idx_int + 1]
    int16_t x1 = points[idx_int].x;
    int16_t y1 = points[idx_int].y;
    int16_t x2 = points[idx_int + 1].x;
    int16_t y2 = points[idx_int + 1].y;

    indicator_point.x = x1 + (int16_t)((x2 - x1) * frac / SCRUB_FIXED_SCALE);
    indicator_point.y = y1 + (int16_t)((y2 - y1) * frac / SCRUB_FIXED_SCALE);
  } else {
    // Normal mode: show at last point
    indicator_point = points[metric->history_count - 1];
  }

  // Draw indicator square at the appropriate position
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(indicator_point.x - 5, indicator_point.y - 5, 10, 10), 0, GCornerNone);
}

//==============================================================================
// Detail Window - Action Button Drawing
//==============================================================================

static void action_button_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Draw semi-circle on right edge (similar to firmware implementation)
  int16_t radius = 12;
  int16_t center_x = bounds.size.w + radius / 2;  // Partially off-screen
  int16_t center_y = bounds.size.h / 2;

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, GPoint(center_x, center_y), radius);
}

//==============================================================================
// Detail Window - Animations
//==============================================================================

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
static void format_fixed_point(int32_t value, int decimals, char *buffer, size_t size) {
  bool negative = value < 0;
  if (negative) value = -value;

  int32_t integer_part = value / FIXED_POINT_SCALE;
  int32_t frac_part = value % FIXED_POINT_SCALE;

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
  int32_t current = s_value_from + (int32_t)(((int64_t)progress * (s_value_to - s_value_from)) / ANIMATION_NORMALIZED_MAX);
  format_fixed_point(current, s_value_decimals, s_animated_value_buffer, sizeof(s_animated_value_buffer));
  text_layer_set_text(s_value_layer, s_animated_value_buffer);
}

static void value_animation_teardown(Animation *animation) {
  // Final update with exact target value
  WandbRun *run = &s_app_data.runs[s_app_data.selected_run_index];
  WandbMetric *metric = &run->metrics[s_app_data.current_metric_page];
  text_layer_set_text(s_value_layer, metric->value);
}

static const AnimationImplementation s_value_animation_impl = {
  .update = value_animation_update,
  .teardown = value_animation_teardown,
};

static Animation *create_value_interpolation_animation(const char *from_value, const char *to_value) {
  int from_decimals, to_decimals;
  s_value_from = parse_fixed_point(from_value, &from_decimals);
  s_value_to = parse_fixed_point(to_value, &to_decimals);
  s_value_decimals = (from_decimals > to_decimals) ? from_decimals : to_decimals;

  Animation *anim = animation_create();
  animation_set_implementation(anim, &s_value_animation_impl);
  animation_set_duration(anim, ANIM_DURATION);
  animation_set_curve(anim, AnimationCurveEaseInOut);
  return anim;
}

static void on_name_outbound_stopped(Animation *animation, bool finished, void *context) {
  // Update name text when outbound animation finishes
  WandbRun *run = &s_app_data.runs[s_app_data.selected_run_index];
  WandbMetric *metric = &run->metrics[s_app_data.current_metric_page];

  to_uppercase(metric->name, s_name_buffer, sizeof(s_name_buffer));
  text_layer_set_text(s_name_layer, s_name_buffer);

  // Update pagination in status bar
  #if !defined(PBL_ROUND)
    snprintf(s_page_buffer, sizeof(s_page_buffer), "%d/%d",
             s_app_data.current_metric_page + 1,
             run->num_metrics);
    text_layer_set_text(s_pagination_layer, s_page_buffer);
  #endif

  // Update graph to show new page data
  s_graph_display_page = s_app_data.current_metric_page;
  if (s_graph_layer) {
    layer_mark_dirty(s_graph_layer);
  }
}

static Animation *create_name_slide_animation(ScrollDirection direction) {
  // Reversed direction: scroll down = text goes up, scroll up = text goes down
  int16_t out_delta = (direction == ScrollDirectionUp) ? ANIM_SLIDE_DISTANCE : -ANIM_SLIDE_DISTANCE;
  int16_t in_delta = -out_delta;

  // Name layer slides out (opposite to scroll direction)
  GRect name_to = s_name_layer_frame;
  name_to.origin.y += out_delta;
  PropertyAnimation *name_out = property_animation_create_layer_frame(
    text_layer_get_layer(s_name_layer), NULL, &name_to);
  animation_set_duration((Animation *)name_out, ANIM_DURATION / 2);
  animation_set_curve((Animation *)name_out, AnimationCurveEaseIn);

  animation_set_handlers((Animation *)name_out, (AnimationHandlers) {
    .stopped = on_name_outbound_stopped,
  }, NULL);

  // Name layer slides in from scroll direction
  GRect name_from = s_name_layer_frame;
  name_from.origin.y += in_delta;
  PropertyAnimation *name_in = property_animation_create_layer_frame(
    text_layer_get_layer(s_name_layer), &name_from, &s_name_layer_frame);
  animation_set_duration((Animation *)name_in, ANIM_DURATION / 2);
  animation_set_curve((Animation *)name_in, AnimationCurveEaseOut);

  return animation_sequence_create((Animation *)name_out, (Animation *)name_in, NULL);
}

static Animation *create_graph_slide_animation(ScrollDirection direction) {
  // Graph slides in the same direction as name (reversed from scroll)
  int16_t out_delta = (direction == ScrollDirectionUp) ? ANIM_SLIDE_DISTANCE : -ANIM_SLIDE_DISTANCE;
  int16_t in_delta = -out_delta;

  // Graph slides out
  GRect graph_to = s_graph_layer_frame;
  graph_to.origin.y += out_delta;
  PropertyAnimation *graph_out = property_animation_create_layer_frame(
    s_graph_layer, NULL, &graph_to);
  animation_set_duration((Animation *)graph_out, ANIM_DURATION / 2);
  animation_set_curve((Animation *)graph_out, AnimationCurveEaseIn);

  // Graph slides in
  GRect graph_from = s_graph_layer_frame;
  graph_from.origin.y += in_delta;
  PropertyAnimation *graph_in = property_animation_create_layer_frame(
    s_graph_layer, &graph_from, &s_graph_layer_frame);
  animation_set_duration((Animation *)graph_in, ANIM_DURATION / 2);
  animation_set_curve((Animation *)graph_in, AnimationCurveEaseOut);

  return animation_sequence_create((Animation *)graph_out, (Animation *)graph_in, NULL);
}

static Animation *create_scroll_animation(ScrollDirection direction, const char *old_value) {
  WandbRun *run = &s_app_data.runs[s_app_data.selected_run_index];
  WandbMetric *metric = &run->metrics[s_app_data.current_metric_page];

  // Value interpolates smoothly
  Animation *value_anim = create_value_interpolation_animation(old_value, metric->value);

  // Name slides with jump animation
  Animation *name_anim = create_name_slide_animation(direction);

  // Graph slides with same animation
  Animation *graph_anim = create_graph_slide_animation(direction);

  return animation_spawn_create(value_anim, name_anim, graph_anim, NULL);
}

static Animation *create_bounce_animation(ScrollDirection direction) {
  // Reversed direction for bounce: scroll down = bounce up, scroll up = bounce down
  int16_t delta = (direction == ScrollDirectionUp) ? ANIM_SLIDE_DISTANCE / 3 : -ANIM_SLIDE_DISTANCE / 3;

  // Name layer bounce
  GRect name_to = s_name_layer_frame;
  name_to.origin.y += delta;
  PropertyAnimation *name_out = property_animation_create_layer_frame(
    text_layer_get_layer(s_name_layer), NULL, &name_to);
  animation_set_duration((Animation *)name_out, ANIM_DURATION / 3);
  animation_set_curve((Animation *)name_out, AnimationCurveEaseOut);

  PropertyAnimation *name_back = property_animation_create_layer_frame(
    text_layer_get_layer(s_name_layer), &name_to, &s_name_layer_frame);
  animation_set_duration((Animation *)name_back, ANIM_DURATION / 3);
  animation_set_curve((Animation *)name_back, AnimationCurveEaseIn);

  Animation *name_bounce = animation_sequence_create((Animation *)name_out, (Animation *)name_back, NULL);

  // Graph layer bounce
  GRect graph_to = s_graph_layer_frame;
  graph_to.origin.y += delta;
  PropertyAnimation *graph_out = property_animation_create_layer_frame(
    s_graph_layer, NULL, &graph_to);
  animation_set_duration((Animation *)graph_out, ANIM_DURATION / 3);
  animation_set_curve((Animation *)graph_out, AnimationCurveEaseOut);

  PropertyAnimation *graph_back = property_animation_create_layer_frame(
    s_graph_layer, &graph_to, &s_graph_layer_frame);
  animation_set_duration((Animation *)graph_back, ANIM_DURATION / 3);
  animation_set_curve((Animation *)graph_back, AnimationCurveEaseIn);

  Animation *graph_bounce = animation_sequence_create((Animation *)graph_out, (Animation *)graph_back, NULL);

  return animation_spawn_create(name_bounce, graph_bounce, NULL);
}

static void do_scroll(ScrollDirection direction) {
  WandbRun *run = &s_app_data.runs[s_app_data.selected_run_index];
  int delta = (direction == ScrollDirectionUp) ? -1 : 1;
  int next_page = s_app_data.current_metric_page + delta;

  Animation *scroll_animation;

  if (next_page < 0 || next_page >= run->num_metrics) {
    // At edge - bounce animation
    scroll_animation = create_bounce_animation(direction);
  } else {
    // Store old value for interpolation
    const char *old_value = run->metrics[s_app_data.current_metric_page].value;

    // Update page
    s_app_data.current_metric_page = next_page;

    // Create scroll animation with old value for interpolation
    scroll_animation = create_scroll_animation(direction, old_value);
  }

  // Unschedule previous animation if any
  if (s_current_animation) {
    animation_unschedule(s_current_animation);
  }

  animation_schedule(scroll_animation);
  s_current_animation = scroll_animation;
}

//==============================================================================
// Detail Window - Scrub Mode
//==============================================================================

// Forward declarations
static void do_scrub(int direction);
static void update_scrub_name_display(void);

static void update_scrub_value_display_interpolated(int32_t index_fixed) {
  WandbRun *run = &s_app_data.runs[s_app_data.selected_run_index];
  WandbMetric *metric = &run->metrics[s_app_data.current_metric_page];

  // Clamp index to valid range for value lookup
  if (index_fixed < 0) index_fixed = 0;
  int32_t max_fixed = (metric->history_count - 1) * SCRUB_FIXED_SCALE;
  if (index_fixed > max_fixed) index_fixed = max_fixed;

  // Get integer and fractional parts
  int idx_int = index_fixed / SCRUB_FIXED_SCALE;
  int32_t frac = index_fixed % SCRUB_FIXED_SCALE;

  // Interpolate between history values
  int32_t history_value;
  if (idx_int >= metric->history_count - 1) {
    history_value = metric->history[metric->history_count - 1];
  } else {
    int32_t v1 = metric->history[idx_int];
    int32_t v2 = metric->history[idx_int + 1];
    history_value = v1 + (v2 - v1) * frac / SCRUB_FIXED_SCALE;
  }

  // Parse the original value to get the decimal count
  int decimals;
  parse_fixed_point(metric->value, &decimals);

  // Format the history value using the same decimal places
  format_fixed_point(history_value, decimals, s_scrub_value_buffer, sizeof(s_scrub_value_buffer));
  text_layer_set_text(s_value_layer, s_scrub_value_buffer);
}

static void scrub_animation_update(Animation *animation, const AnimationProgress progress) {
  s_scrub_current_index_fixed = s_scrub_from_index_fixed +
    (int32_t)(((int64_t)progress * (s_scrub_to_index_fixed - s_scrub_from_index_fixed)) / ANIMATION_NORMALIZED_MAX);

  update_scrub_value_display_interpolated(s_scrub_current_index_fixed);
  layer_mark_dirty(s_graph_layer);
}

static void scrub_animation_teardown(Animation *animation) {
  // Snap to final position
  s_scrub_current_index_fixed = s_scrub_to_index_fixed;

  // Clamp to valid range
  WandbRun *run = &s_app_data.runs[s_app_data.selected_run_index];
  WandbMetric *metric = &run->metrics[s_app_data.current_metric_page];
  int32_t max_fixed = (metric->history_count - 1) * SCRUB_FIXED_SCALE;

  if (s_scrub_current_index_fixed < 0) s_scrub_current_index_fixed = 0;
  if (s_scrub_current_index_fixed > max_fixed) s_scrub_current_index_fixed = max_fixed;

  s_scrub_index = s_scrub_current_index_fixed / SCRUB_FIXED_SCALE;
  update_scrub_value_display_interpolated(s_scrub_current_index_fixed);
  update_scrub_name_display();
  layer_mark_dirty(s_graph_layer);
  s_scrub_animation = NULL;
}

static const AnimationImplementation s_scrub_animation_impl = {
  .update = scrub_animation_update,
  .teardown = scrub_animation_teardown,
};

// Bounce animation state
static int32_t s_bounce_target_fixed;
static int32_t s_bounce_return_fixed;

static void bounce_animation_update(Animation *animation, const AnimationProgress progress) {
  // First half: go to bounce target, second half: return
  if (progress < ANIMATION_NORMALIZED_MAX / 2) {
    // Going out
    AnimationProgress out_progress = progress * 2;
    s_scrub_current_index_fixed = s_scrub_from_index_fixed +
      (int32_t)(((int64_t)out_progress * (s_bounce_target_fixed - s_scrub_from_index_fixed)) / ANIMATION_NORMALIZED_MAX);
  } else {
    // Coming back
    AnimationProgress back_progress = (progress - ANIMATION_NORMALIZED_MAX / 2) * 2;
    s_scrub_current_index_fixed = s_bounce_target_fixed +
      (int32_t)(((int64_t)back_progress * (s_bounce_return_fixed - s_bounce_target_fixed)) / ANIMATION_NORMALIZED_MAX);
  }

  update_scrub_value_display_interpolated(s_scrub_current_index_fixed);
  layer_mark_dirty(s_graph_layer);
}

static void bounce_animation_teardown(Animation *animation) {
  s_scrub_current_index_fixed = s_bounce_return_fixed;
  update_scrub_value_display_interpolated(s_scrub_current_index_fixed);
  layer_mark_dirty(s_graph_layer);
  s_scrub_animation = NULL;
}

static const AnimationImplementation s_bounce_animation_impl = {
  .update = bounce_animation_update,
  .teardown = bounce_animation_teardown,
};

static void do_scrub(int direction) {
  WandbRun *run = &s_app_data.runs[s_app_data.selected_run_index];
  WandbMetric *metric = &run->metrics[s_app_data.current_metric_page];

  int32_t target_index = s_scrub_index + direction;
  int32_t max_index = metric->history_count - 1;

  // Unschedule previous animation and snap to its target
  if (s_scrub_animation) {
    animation_unschedule(s_scrub_animation);
    // Snap to the target of the cancelled animation
    s_scrub_current_index_fixed = s_scrub_to_index_fixed;
    s_scrub_animation = NULL;
  }

  // Check if at edge - do bounce animation
  if (target_index < 0 || target_index > max_index) {
    // Bounce animation: go slightly past edge then spring back
    int32_t bounce_amount = SCRUB_FIXED_SCALE / 3;  // 1/3 of a step

    s_scrub_from_index_fixed = s_scrub_current_index_fixed;
    if (target_index < 0) {
      s_bounce_target_fixed = -bounce_amount;
      s_bounce_return_fixed = 0;
    } else {
      s_bounce_target_fixed = max_index * SCRUB_FIXED_SCALE + bounce_amount;
      s_bounce_return_fixed = max_index * SCRUB_FIXED_SCALE;
    }

    s_scrub_animation = animation_create();
    animation_set_implementation(s_scrub_animation, &s_bounce_animation_impl);
    animation_set_duration(s_scrub_animation, SCRUB_ANIM_DURATION);
    animation_set_curve(s_scrub_animation, AnimationCurveEaseOut);
    animation_schedule(s_scrub_animation);
    return;
  }

  // Normal scrub: animate to target
  s_scrub_from_index_fixed = s_scrub_current_index_fixed;
  s_scrub_to_index_fixed = target_index * SCRUB_FIXED_SCALE;
  s_scrub_index = target_index;

  s_scrub_animation = animation_create();
  animation_set_implementation(s_scrub_animation, &s_scrub_animation_impl);
  animation_set_duration(s_scrub_animation, SCRUB_ANIM_DURATION);
  animation_set_curve(s_scrub_animation, AnimationCurveEaseInOut);
  animation_schedule(s_scrub_animation);
}

// Wiggle animation state
static int32_t s_wiggle_start_fixed;
static int32_t s_wiggle_amount;
#define WIGGLE_ANIM_DURATION 300

static void wiggle_animation_update(Animation *animation, const AnimationProgress progress) {
  // Wiggle: go left, then right, then back to center
  // Using a sine-like pattern: 0 -> -1 -> 0 -> +0.5 -> 0
  int32_t offset;
  if (progress < ANIMATION_NORMALIZED_MAX / 3) {
    // First third: go left
    AnimationProgress p = progress * 3;
    offset = -(int32_t)(((int64_t)p * s_wiggle_amount) / ANIMATION_NORMALIZED_MAX);
  } else if (progress < 2 * ANIMATION_NORMALIZED_MAX / 3) {
    // Second third: go from left to right
    AnimationProgress p = (progress - ANIMATION_NORMALIZED_MAX / 3) * 3;
    offset = -s_wiggle_amount + (int32_t)(((int64_t)p * s_wiggle_amount * 3 / 2) / ANIMATION_NORMALIZED_MAX);
  } else {
    // Final third: return to center
    AnimationProgress p = (progress - 2 * ANIMATION_NORMALIZED_MAX / 3) * 3;
    int32_t start_offset = s_wiggle_amount / 2;
    offset = start_offset - (int32_t)(((int64_t)p * start_offset) / ANIMATION_NORMALIZED_MAX);
  }

  s_scrub_current_index_fixed = s_wiggle_start_fixed + offset;
  // Only animate the dot, not the value
  layer_mark_dirty(s_graph_layer);
}

static void wiggle_animation_teardown(Animation *animation) {
  s_scrub_current_index_fixed = s_wiggle_start_fixed;
  update_scrub_value_display_interpolated(s_scrub_current_index_fixed);
  layer_mark_dirty(s_graph_layer);
  s_scrub_animation = NULL;
}

static const AnimationImplementation s_wiggle_animation_impl = {
  .update = wiggle_animation_update,
  .teardown = wiggle_animation_teardown,
};

static void update_scrub_name_display(void) {
  WandbRun *run = &s_app_data.runs[s_app_data.selected_run_index];
  WandbMetric *metric = &run->metrics[s_app_data.current_metric_page];

  to_uppercase(metric->name, s_name_buffer, sizeof(s_name_buffer));
  // Show "(PAST)" only when viewing a historical point, not the latest
  if (s_scrub_mode && s_scrub_index < metric->history_count - 1) {
    strncat(s_name_buffer, " (PAST)", sizeof(s_name_buffer) - strlen(s_name_buffer) - 1);
  }
  text_layer_set_text(s_name_layer, s_name_buffer);
}

static void enter_scrub_mode(void) {
  WandbRun *run = &s_app_data.runs[s_app_data.selected_run_index];
  WandbMetric *metric = &run->metrics[s_app_data.current_metric_page];

  s_scrub_mode = true;
  s_scrub_index = metric->history_count - 1;  // Start at the last (current) value
  s_scrub_current_index_fixed = s_scrub_index * SCRUB_FIXED_SCALE;

  update_scrub_value_display_interpolated(s_scrub_current_index_fixed);
  update_scrub_name_display();
  layer_mark_dirty(s_graph_layer);
  layer_mark_dirty(s_action_button_layer);

  // Start wiggle animation to indicate scrub mode
  s_wiggle_start_fixed = s_scrub_current_index_fixed;
  s_wiggle_amount = SCRUB_FIXED_SCALE;  // Wiggle by 1 data point

  s_scrub_animation = animation_create();
  animation_set_implementation(s_scrub_animation, &s_wiggle_animation_impl);
  animation_set_duration(s_scrub_animation, WIGGLE_ANIM_DURATION);
  animation_set_curve(s_scrub_animation, AnimationCurveLinear);
  animation_schedule(s_scrub_animation);
}

static void exit_scrub_animation_teardown(Animation *animation) {
  s_scrub_mode = false;
  s_scrub_animation = NULL;

  // Restore the actual metric value and name
  WandbRun *run = &s_app_data.runs[s_app_data.selected_run_index];
  WandbMetric *metric = &run->metrics[s_app_data.current_metric_page];
  text_layer_set_text(s_value_layer, metric->value);

  to_uppercase(metric->name, s_name_buffer, sizeof(s_name_buffer));
  text_layer_set_text(s_name_layer, s_name_buffer);

  layer_mark_dirty(s_graph_layer);
  layer_mark_dirty(s_action_button_layer);
}

static const AnimationImplementation s_exit_scrub_animation_impl = {
  .update = scrub_animation_update,
  .teardown = exit_scrub_animation_teardown,
};

static void exit_scrub_mode(void) {
  // Cancel repeat timer
  if (s_scrub_repeat_timer) {
    app_timer_cancel(s_scrub_repeat_timer);
    s_scrub_repeat_timer = NULL;
  }
  s_scrub_repeat_direction = 0;

  // Cancel any running scrub animation
  if (s_scrub_animation) {
    animation_unschedule(s_scrub_animation);
    s_scrub_animation = NULL;
  }

  WandbRun *run = &s_app_data.runs[s_app_data.selected_run_index];
  WandbMetric *metric = &run->metrics[s_app_data.current_metric_page];
  int32_t target_index_fixed = (metric->history_count - 1) * SCRUB_FIXED_SCALE;

  // Always animate back to the end position (even if already there)
  // This ensures dots stay visible until animation completes
  s_scrub_from_index_fixed = s_scrub_current_index_fixed;
  s_scrub_to_index_fixed = target_index_fixed;

  s_scrub_animation = animation_create();
  animation_set_implementation(s_scrub_animation, &s_exit_scrub_animation_impl);
  animation_set_duration(s_scrub_animation, SCRUB_ANIM_DURATION * 2);  // Slightly longer for exit
  animation_set_curve(s_scrub_animation, AnimationCurveEaseOut);
  animation_schedule(s_scrub_animation);
}

//==============================================================================
// Detail Window - Click Handlers
//==============================================================================

static void scrub_repeat_timer_callback(void *data) {
  if (s_scrub_mode && s_scrub_repeat_direction != 0) {
    do_scrub(s_scrub_repeat_direction);
    s_scrub_repeat_timer = app_timer_register(SCRUB_REPEAT_INTERVAL, scrub_repeat_timer_callback, NULL);
  } else {
    s_scrub_repeat_timer = NULL;
  }
}

static void stop_scrub_repeat(void) {
  if (s_scrub_repeat_timer) {
    app_timer_cancel(s_scrub_repeat_timer);
    s_scrub_repeat_timer = NULL;
  }
  s_scrub_repeat_direction = 0;
}

static void detail_up_down_handler(ClickRecognizerRef recognizer, void *context) {
  ButtonId button = click_recognizer_get_button_id(recognizer);
  int direction = (button == BUTTON_ID_UP) ? 1 : -1;

  if (s_scrub_mode) {
    // Cancel any existing timer (but don't reset direction yet)
    if (s_scrub_repeat_timer) {
      app_timer_cancel(s_scrub_repeat_timer);
    }

    // Immediate scrub on press, then start repeat timer
    do_scrub(direction);
    s_scrub_repeat_direction = direction;
    s_scrub_repeat_timer = app_timer_register(SCRUB_REPEAT_INTERVAL, scrub_repeat_timer_callback, NULL);
  } else {
    do_scroll((button == BUTTON_ID_UP) ? ScrollDirectionUp : ScrollDirectionDown);
  }
}

static void detail_up_down_release_handler(ClickRecognizerRef recognizer, void *context) {
  stop_scrub_repeat();
}

static void detail_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_scrub_mode) {
    exit_scrub_mode();
  } else {
    enter_scrub_mode();
  }
}

static void detail_click_config_provider(void *context) {
  // Use raw handlers for UP/DOWN to get immediate response without delay
  window_raw_click_subscribe(BUTTON_ID_UP, detail_up_down_handler, detail_up_down_release_handler, NULL);
  window_raw_click_subscribe(BUTTON_ID_DOWN, detail_up_down_handler, detail_up_down_release_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_SELECT, detail_select_click_handler);
}

//==============================================================================
// Detail Window - Lifecycle
//==============================================================================

static void detail_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  // Create status bar
  s_detail_status_bar = status_bar_layer_create();
  status_bar_layer_set_colors(s_detail_status_bar, GColorClear, GColorBlack);
  status_bar_layer_set_separator_mode(s_detail_status_bar, StatusBarLayerSeparatorModeDotted);
  layer_add_child(window_layer, status_bar_layer_get_layer(s_detail_status_bar));

  // Create pagination layer (right-aligned in status bar area, hidden on round)
  #if !defined(PBL_ROUND)
    #if defined(PBL_PLATFORM_EMERY)
      s_pagination_layer = text_layer_create(GRect(bounds.size.w - 50, -2, 46, 22));
      text_layer_set_font(s_pagination_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
    #else
      s_pagination_layer = text_layer_create(GRect(bounds.size.w - 40, -2, 36, STATUS_BAR_HEIGHT));
      text_layer_set_font(s_pagination_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
    #endif
    text_layer_set_text_alignment(s_pagination_layer, GTextAlignmentRight);
    text_layer_set_background_color(s_pagination_layer, GColorClear);
    layer_add_child(window_layer, text_layer_get_layer(s_pagination_layer));
  #endif

  int16_t padding_left;
  int16_t value_y;
  int16_t name_y;
  int16_t content_width;

  #if defined(PBL_ROUND)
    padding_left = 30;
    name_y = STATUS_BAR_HEIGHT + padding_left / 2;  // Half top padding for round
    value_y = name_y + 22;  // Below name layer (22px height for GOTHIC_18)
    content_width = bounds.size.w - padding_left * 2;
  #else
    padding_left = PADDING_LEFT;
    name_y = STATUS_BAR_HEIGHT + padding_left;
    value_y = name_y + 22;  // Below name layer (22px height for GOTHIC_18)
    content_width = bounds.size.w - padding_left;
  #endif

  #if defined(PBL_ROUND)
    GTextAlignment text_align = GTextAlignmentCenter;
  #else
    GTextAlignment text_align = GTextAlignmentLeft;
  #endif

  // Store original frames for animation
  s_name_layer_frame = GRect(padding_left, name_y, content_width, 22);
  s_value_layer_frame = GRect(padding_left, value_y, content_width, 32);

  // Calculate graph position (below value, fill remaining space) - double padding
  int16_t graph_y = value_y + 32 + PADDING_LEFT * 2;  // Below value layer with double padding
  #if defined(PBL_ROUND)
    int16_t graph_height = bounds.size.h - graph_y - padding_left;  // More bottom padding for round
    s_graph_layer_frame = GRect(padding_left + 10, graph_y, content_width - 20, graph_height);
  #else
    int16_t graph_height = bounds.size.h - graph_y - PADDING_LEFT;  // Fill to bottom with padding
    s_graph_layer_frame = GRect(padding_left, graph_y, content_width - PADDING_LEFT, graph_height);
  #endif

  // Name layer - bold gothic font at top (uppercase)
  s_name_layer = text_layer_create(s_name_layer_frame);
  text_layer_set_font(s_name_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_name_layer, text_align);
  text_layer_set_background_color(s_name_layer, GColorClear);
  layer_add_child(window_layer, text_layer_get_layer(s_name_layer));

  // Value layer - LECO numbers font below name
  s_value_layer = text_layer_create(s_value_layer_frame);
  text_layer_set_font(s_value_layer, fonts_get_system_font(FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM));
  text_layer_set_text_alignment(s_value_layer, text_align);
  text_layer_set_background_color(s_value_layer, GColorClear);
  layer_add_child(window_layer, text_layer_get_layer(s_value_layer));

  // Graph layer - draws the metric history
  s_graph_layer = layer_create(s_graph_layer_frame);
  layer_set_update_proc(s_graph_layer, graph_layer_update_proc);
  layer_set_clips(s_graph_layer, false);
  layer_add_child(window_layer, s_graph_layer);

  // Action button layer - full window, draws semi-circle on right edge when in scrub mode
  s_action_button_layer = layer_create(bounds);
  layer_set_update_proc(s_action_button_layer, action_button_layer_update_proc);
  layer_add_child(window_layer, s_action_button_layer);

  // Reset animation and scrub mode state
  s_current_animation = NULL;
  s_scrub_mode = false;

  update_detail_text();
}

static void detail_window_unload(Window *window) {
  // Reset scrub mode
  s_scrub_mode = false;

  if (s_current_animation) {
    animation_unschedule(s_current_animation);
    s_current_animation = NULL;
  }
  text_layer_destroy(s_value_layer);
  text_layer_destroy(s_name_layer);
  layer_destroy(s_graph_layer);
  layer_destroy(s_action_button_layer);
  #if !defined(PBL_ROUND)
    text_layer_destroy(s_pagination_layer);
  #endif
  status_bar_layer_destroy(s_detail_status_bar);
  window_destroy(s_detail_window);
  s_detail_window = NULL;
}

static void detail_window_push(void) {
  s_detail_window = window_create();
  window_set_click_config_provider(s_detail_window, detail_click_config_provider);
  window_set_window_handlers(s_detail_window, (WindowHandlers) {
    .load = detail_window_load,
    .unload = detail_window_unload,
  });
  window_stack_push(s_detail_window, true);
}

//==============================================================================
// Main Menu Window
//==============================================================================

static uint16_t menu_get_num_sections_callback(MenuLayer *menu_layer, void *data) {
  return 1;
}

static uint16_t menu_get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
  return s_app_data.num_runs;
}


static void menu_draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
  WandbRun *run = &s_app_data.runs[cell_index->row];
  menu_cell_basic_draw(ctx, cell_layer, run->run_name, run->project_name, NULL);
}

static void menu_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  s_app_data.selected_run_index = cell_index->row;
  s_app_data.current_metric_page = 0;
  detail_window_push();
}

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  // Create menu layer below status bar
  GRect menu_bounds = GRect(0, STATUS_BAR_HEIGHT, bounds.size.w, bounds.size.h - STATUS_BAR_HEIGHT);
  s_menu_layer = menu_layer_create(menu_bounds);

  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_sections = menu_get_num_sections_callback,
    .get_num_rows = menu_get_num_rows_callback,
    .draw_row = menu_draw_row_callback,
    .select_click = menu_select_callback,
  });

  menu_layer_set_click_config_onto_window(s_menu_layer, window);

  #if defined(PBL_COLOR)
    menu_layer_set_normal_colors(s_menu_layer, GColorWhite, GColorBlack);
    menu_layer_set_highlight_colors(s_menu_layer, GColorBlack, GColorWhite);
  #endif

  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));

  // Create status bar (added last so it draws on top)
  s_main_status_bar = status_bar_layer_create();
  status_bar_layer_set_colors(s_main_status_bar, GColorClear, GColorBlack);
  status_bar_layer_set_separator_mode(s_main_status_bar, StatusBarLayerSeparatorModeDotted);
  layer_add_child(window_layer, status_bar_layer_get_layer(s_main_status_bar));
}

static void main_window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
  status_bar_layer_destroy(s_main_status_bar);
}

//==============================================================================
// App Lifecycle
//==============================================================================

static void prv_init(void) {
  init_mock_data();

  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);
}

static void prv_deinit(void) {
  window_destroy(s_main_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
