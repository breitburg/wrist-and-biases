var Clay = require('pebble-clay');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig);

// Load settings from localStorage
var settings = localStorage.getItem('clay-settings');
var config = settings ? JSON.parse(settings) : {};

// Module-level state
var runs = [];
var client = new WandbClient(config.apiKey, config.baseUrl);

// Cache for sorted metric names (NOT values - those are fetched fresh)
var cachedMetricNames = {
  runKey: null,           // "entity/project/runName"
  names: []               // Array of metric names in sorted order
};

function base64Encode(str) {
  var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=';
  var output = '';

  for (var i = 0; i < str.length; i += 3) {
    var b1 = str.charCodeAt(i);
    var b2 = i + 1 < str.length ? str.charCodeAt(i + 1) : 0;
    var b3 = i + 2 < str.length ? str.charCodeAt(i + 2) : 0;

    var e1 = b1 >> 2;
    var e2 = ((b1 & 3) << 4) | (b2 >> 4);
    var e3 = ((b2 & 15) << 2) | (b3 >> 6);
    var e4 = b3 & 63;

    if (i + 1 >= str.length) e3 = e4 = 64;
    else if (i + 2 >= str.length) e4 = 64;

    output += chars.charAt(e1) + chars.charAt(e2) + chars.charAt(e3) + chars.charAt(e4);
  }

  return output;
}

function WandbClient(apiKey, baseUrl) {
  this.apiKey = apiKey;
  this.endpoint = baseUrl || 'https://api.wandb.ai/graphql';
}

WandbClient.prototype.request = function (query, variables, callback, retryCount) {
  var self = this;
  var maxRetries = 3;
  var currentRetry = retryCount || 0;

  var xhr = new XMLHttpRequest();
  xhr.open('POST', this.endpoint, true);
  xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.setRequestHeader('Authorization', 'Basic ' + base64Encode('api:' + this.apiKey));

  xhr.onreadystatechange = function () {
    if (xhr.readyState !== 4) return;

    if (xhr.status !== 200) {
      // Retry on server errors (5xx) or timeout
      if ((xhr.status >= 500 || xhr.status === 0) && currentRetry < maxRetries) {
        console.log('Request failed with HTTP ' + xhr.status + ', retrying (' + (currentRetry + 1) + '/' + maxRetries + ')');
        setTimeout(function() {
          self.request(query, variables, callback, currentRetry + 1);
        }, 1000 * (currentRetry + 1)); // Exponential backoff
        return;
      }
      return callback('HTTP ' + xhr.status, null);
    }

    try {
      var response = JSON.parse(xhr.responseText);
      if (response.errors) return callback(response.errors, null);
      callback(null, response.data);
    } catch (e) {
      console.log('JSON parse error: ' + e.message);
      if (currentRetry < maxRetries) {
        console.log('Retrying after parse error (' + (currentRetry + 1) + '/' + maxRetries + ')');
        setTimeout(function() {
          self.request(query, variables, callback, currentRetry + 1);
        }, 1000 * (currentRetry + 1));
        return;
      }
      callback('JSON parse error: ' + e.message, null);
    }
  };

  xhr.onerror = function () {
    if (currentRetry < maxRetries) {
      console.log('Network error, retrying (' + (currentRetry + 1) + '/' + maxRetries + ')');
      setTimeout(function() {
        self.request(query, variables, callback, currentRetry + 1);
      }, 1000 * (currentRetry + 1));
      return;
    }
    callback('Network error', null);
  };

  var body = { query: query };
  if (variables) body.variables = variables;

  try {
    xhr.send(JSON.stringify(body));
  } catch (e) {
    console.log('Send error: ' + e.message);
    if (currentRetry < maxRetries) {
      setTimeout(function() {
        self.request(query, variables, callback, currentRetry + 1);
      }, 1000 * (currentRetry + 1));
      return;
    }
    callback('Send error: ' + e.message, null);
  }
};

WandbClient.prototype.fetchViewer = function (callback) {
  var query = 'query { viewer { entity username } }';
  this.request(query, null, callback);
};

WandbClient.prototype.fetchProjects = function (entity, callback) {
  var query = 'query($entity: String!) { models(entityName: $entity, first: 100) { edges { node { name entityName } } } }';
  this.request(query, { entity: entity }, callback);
};

WandbClient.prototype.fetchRuns = function (entity, project, callback) {
  var query = 'query($entity: String!, $project: String!) { project(name: $project, entityName: $entity) { runs(first: 5, order: "-createdAt") { edges { node { name displayName state createdAt } } } } }';
  this.request(query, { entity: entity, project: project }, callback);
};

WandbClient.prototype.fetchAllRuns = function (callback) {
  var self = this;

  this.fetchViewer(function (err, data) {
    if (err) return callback(err, null);

    var entity = data.viewer.entity;

    self.fetchProjects(entity, function (err, data) {
      if (err) return callback(err, null);

      var allProjects = [];
      if (data.models) {
        data.models.edges.forEach(function (edge) {
          allProjects.push({ entity: edge.node.entityName, name: edge.node.name });
        });
      }

      if (allProjects.length === 0) return callback(null, []);

      var allRuns = [];
      var pendingProjects = allProjects.length;

      allProjects.forEach(function (project) {
        self.fetchRuns(project.entity, project.name, function (err, data) {
          if (!err && data.project && data.project.runs) {
            data.project.runs.edges.forEach(function (edge) {
              allRuns.push({ entity: project.entity, project: project.name, run: edge.node });
            });
          }

          pendingProjects--;
          if (pendingProjects === 0) callback(null, allRuns);
        });
      });
    });
  });
};

// Fetch only metric names (for sorting/caching) - lightweight call
WandbClient.prototype.fetchMetricNames = function (entity, project, runName, callback) {
  var query = 'query($entity: String!, $project: String!, $runName: String!) { ' +
    'project(name: $project, entityName: $entity) { ' +
      'run(name: $runName) { ' +
        'summaryMetrics historyKeys ' +
      '} ' +
    '} ' +
  '}';

  this.request(query, { entity: entity, project: project, runName: runName }, function(err, data) {
    if (err) return callback(err, null);

    try {
      var run = data.project.run;
      var summary = JSON.parse(run.summaryMetrics);
      var historyKeysAvailable = run.historyKeys && run.historyKeys.keys
        ? Object.keys(run.historyKeys.keys)
        : [];

      // Build list of metric names with metadata for sorting
      var metrics = [];
      var keys = Object.keys(summary);

      for (var i = 0; i < keys.length; i++) {
        var key = keys[i];
        if (key.charAt(0) === '_') continue;
        if (typeof summary[key] !== 'number') continue;

        var hasHistory = historyKeysAvailable.indexOf(key) !== -1 &&
                         key.indexOf('system/') !== 0;

        metrics.push({ name: key, hasHistory: hasHistory });
      }

      // Sort: loss/accuracy priority, then by history availability, then alphabetically
      var priorities = ['loss', 'accuracy'];

      function getPriority(name) {
        var lower = name.toLowerCase();
        for (var j = 0; j < priorities.length; j++) {
          if (lower.indexOf(priorities[j]) !== -1) return j;
        }
        return priorities.length;
      }

      metrics.sort(function(a, b) {
        var aPriority = getPriority(a.name);
        var bPriority = getPriority(b.name);
        if (aPriority !== bPriority) return aPriority - bPriority;

        var aHasHistory = a.hasHistory ? 0 : 1;
        var bHasHistory = b.hasHistory ? 0 : 1;
        if (aHasHistory !== bHasHistory) return aHasHistory - bHasHistory;

        return a.name < b.name ? -1 : 1;
      });

      // Return just the sorted names
      var sortedNames = metrics.map(function(m) { return m.name; });
      callback(null, sortedNames);
    } catch (e) {
      console.log('Error processing metric names: ' + e.message);
      callback('Error processing metric names: ' + e.message, null);
    }
  });
};

// Fetch a single metric's current value and history
WandbClient.prototype.fetchSingleMetric = function (entity, project, runName, metricName, callback) {
  var specs = [JSON.stringify({ keys: ['_step', metricName], samples: 20 })];

  var query = 'query($entity: String!, $project: String!, $runName: String!, $specs: [JSONString!]!) { ' +
    'project(name: $project, entityName: $entity) { ' +
      'run(name: $runName) { ' +
        'summaryMetrics sampledHistory(specs: $specs) ' +
      '} ' +
    '} ' +
  '}';

  this.request(query, { entity: entity, project: project, runName: runName, specs: specs }, function(err, data) {
    if (err) return callback(err, null);

    try {
      var run = data.project.run;
      var summary = JSON.parse(run.summaryMetrics);
      var value = summary[metricName];

      // Format value for display
      var displayValue;
      if (value === undefined || value === null) {
        displayValue = '---';
      } else if (Math.abs(value) >= 1000) {
        displayValue = value.toFixed(0);
      } else if (Math.abs(value) >= 1) {
        displayValue = value.toFixed(2);
      } else {
        displayValue = value.toFixed(4);
      }

      // Parse history
      var sampledHistory = run.sampledHistory;
      var rows = sampledHistory[0] || [];
      var history = [];

      for (var j = 0; j < rows.length; j++) {
        var val = rows[j][metricName];
        if (val !== undefined && val !== null) {
          history.push(toFixedPoint(val));
        }
      }

      callback(null, { name: metricName, value: displayValue, history: history });
    } catch (e) {
      console.log('Error processing metric data: ' + e.message);
      callback('Error processing metric data: ' + e.message, null);
    }
  });
};

function sendRunsToWatch(runs) {
  if (runs.length === 0) {
    Pebble.sendAppMessage({ 'RUNS_COUNT': 0 }, function () {}, function (err) {
      console.log('Failed to send empty count: ' + JSON.stringify(err));
    });
    return;
  }

  runs.sort(function (a, b) {
    if (a.run.state === 'running' && b.run.state !== 'running') return -1;
    if (a.run.state !== 'running' && b.run.state === 'running') return 1;
    if (a.run.state < b.run.state) return -1;
    if (a.run.state > b.run.state) return 1;
    return 0;
  });

  var index = 0;

  function sendNext() {
    if (index >= runs.length) return;

    var item = runs[index];
    var message = {
      'RUN_NAME': item.run.displayName || item.run.name,
      'RUN_OWNER': item.entity + '/' + item.project,
      'RUN_STATE': item.run.state
    };

    if (index === 0) {
      message['RUNS_COUNT'] = runs.length;
    }

    Pebble.sendAppMessage(message, function () {
      index++;
      sendNext();
    }, function (err) {
      console.log('Failed to send run: ' + JSON.stringify(err));
    });
  }

  sendNext();
}

function toFixedPoint(value) {
  return Math.round(value * 10000);
}

function packInt64Array(values) {
  var buffer = new ArrayBuffer(values.length * 8);
  var view = new DataView(buffer);
  for (var i = 0; i < values.length; i++) {
    // Split 64-bit value into low and high 32-bit parts
    var val = values[i];
    var negative = val < 0;
    if (negative) val = -val;
    var low = val >>> 0;  // Lower 32 bits
    var high = Math.floor(val / 4294967296) >>> 0;  // Upper 32 bits
    if (negative) {
      // Two's complement for negative numbers
      low = (~low + 1) >>> 0;
      high = (~high + (low === 0 ? 1 : 0)) >>> 0;
    }
    view.setUint32(i * 8, low, true);
    view.setUint32(i * 8 + 4, high, true);
  }
  var uint8 = new Uint8Array(buffer);
  var result = [];
  for (var j = 0; j < uint8.length; j++) {
    result.push(uint8[j]);
  }
  return result;
}

// Send a single metric to the watch
function sendMetricToWatch(metric, metricIndex, totalCount) {
  var message = {
    'METRIC_NAME': metric.name,
    'METRIC_VALUE': metric.value,
    'METRIC_INDEX': metricIndex,
    'METRICS_COUNT': totalCount
  };

  if (metric.history && metric.history.length > 0) {
    message['METRIC_HISTORY'] = packInt64Array(metric.history);
  }

  Pebble.sendAppMessage(message, function () {
    console.log('Sent metric ' + metricIndex + ': ' + metric.name);
  }, function (err) {
    console.log('Failed to send metric ' + metricIndex + ': ' + JSON.stringify(err));
  });
}

// Fetch and send a metric by index (uses cached names if available)
function fetchAndSendMetric(runInfo, metricIndex) {
  var runKey = runInfo.entity + '/' + runInfo.project + '/' + runInfo.run.name;

  function doFetch(names) {
    if (metricIndex >= names.length) {
      console.log('Metric index ' + metricIndex + ' out of bounds (total: ' + names.length + ')');
      return;
    }

    var metricName = names[metricIndex];
    client.fetchSingleMetric(runInfo.entity, runInfo.project, runInfo.run.name, metricName, function(err, metric) {
      if (err) {
        console.log('Error fetching metric: ' + JSON.stringify(err));
        return;
      }
      sendMetricToWatch(metric, metricIndex, names.length);
    });
  }

  // Check if we have cached names for this run
  if (cachedMetricNames.runKey === runKey) {
    doFetch(cachedMetricNames.names);
  } else {
    // Need to fetch names first
    client.fetchMetricNames(runInfo.entity, runInfo.project, runInfo.run.name, function(err, names) {
      if (err) {
        console.log('Error fetching metric names: ' + JSON.stringify(err));
        return;
      }
      // Cache the names
      cachedMetricNames.runKey = runKey;
      cachedMetricNames.names = names;
      doFetch(names);
    });
  }
}

Pebble.addEventListener('ready', function () {
  console.log('PebbleKit JS ready');

  if (!config.apiKey) {
    console.log('No API key configured');
    return;
  }

  client.fetchAllRuns(function (err, fetchedRuns) {
    if (err) {
      console.log('Error fetching runs: ' + JSON.stringify(err));
      return;
    }

    console.log('Fetched ' + fetchedRuns.length + ' runs');
    runs = fetchedRuns;
    sendRunsToWatch(runs);
  });
});

Pebble.addEventListener('appmessage', function (e) {
  var runIndex = e.payload['FETCH_RUN_INDEX'];
  var metricIndex = e.payload['FETCH_METRIC_INDEX'];

  if (runIndex !== undefined && runIndex !== null && metricIndex !== undefined && metricIndex !== null) {
    var runInfo = runs[runIndex];
    console.log('Fetching metric ' + metricIndex + ' for run ' + runIndex);
    fetchAndSendMetric(runInfo, metricIndex);
  }
});
