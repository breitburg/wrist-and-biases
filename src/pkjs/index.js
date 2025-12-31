var Clay = require('pebble-clay');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig);

// Module-level cache for runs lookup
var cachedRuns = [];
var cachedClient = null;

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

WandbClient.prototype.request = function (query, variables, callback) {
  var xhr = new XMLHttpRequest();
  xhr.open('POST', this.endpoint, true);
  xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.setRequestHeader('Authorization', 'Basic ' + base64Encode('api:' + this.apiKey));

  xhr.onreadystatechange = function () {
    if (xhr.readyState !== 4) return;
    if (xhr.status !== 200) return callback('HTTP ' + xhr.status, null);

    var response = JSON.parse(xhr.responseText);
    if (response.errors) return callback(response.errors, null);
    callback(null, response.data);
  };

  xhr.onerror = function () {
    callback('Network error', null);
  };

  var body = { query: query };
  if (variables) body.variables = variables;
  xhr.send(JSON.stringify(body));
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

WandbClient.prototype.fetchRunMetrics = function (entity, project, runName, callback) {
  var self = this;
  var summaryQuery = 'query($entity: String!, $project: String!, $runName: String!) { ' +
    'project(name: $project, entityName: $entity) { ' +
      'run(name: $runName) { ' +
        'summaryMetrics ' +
      '} ' +
    '} ' +
  '}';

  this.request(summaryQuery, { entity: entity, project: project, runName: runName }, function(err, data) {
    if (err) return callback(err, null);

    var run = data.project.run;

    var historyQuery = 'query($entity: String!, $project: String!, $runName: String!) { ' +
      'project(name: $project, entityName: $entity) { ' +
        'run(name: $runName) { ' +
          'history(samples: 20) ' +
        '} ' +
      '} ' +
    '}';

    self.request(historyQuery, { entity: entity, project: project, runName: runName }, function(err2, historyData) {
      if (err2) return callback(err2, null);

      var rawHistory = historyData.project.run.history;
      var history;

      if (Array.isArray(rawHistory)) {
        history = rawHistory;
      } else if (typeof rawHistory === 'string') {
        var histStr = rawHistory.trim();
        if (histStr.charAt(0) === '{') {
          histStr = '[' + histStr.replace(/\}\s*\{/g, '},{') + ']';
        }
        history = JSON.parse(histStr);
      } else {
        var jsonStr = JSON.stringify(rawHistory);
        if (jsonStr.charAt(0) === '{') {
          jsonStr = '[' + jsonStr.replace(/\}\s*\{/g, '},{') + ']';
        }
        history = JSON.parse(jsonStr);
      }

      callback(null, { project: { run: { summaryMetrics: run.summaryMetrics, sampledHistory: history } } });
    });
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

function sendMetricsToWatch(metrics) {
  if (metrics.length === 0) {
    Pebble.sendAppMessage({ 'METRICS_COUNT': 0 }, function () {}, function (err) {
      console.log('Failed to send empty metrics count: ' + JSON.stringify(err));
    });
    return;
  }

  var index = 0;

  function sendNext() {
    if (index >= metrics.length) return;

    var metric = metrics[index];
    var message = {
      'METRIC_NAME': metric.name,
      'METRIC_VALUE': metric.value
    };

    if (metric.history && metric.history.length > 0) {
      message['METRIC_HISTORY'] = packInt64Array(metric.history);
    }

    if (index === 0) {
      message['METRICS_COUNT'] = metrics.length;
    }

    Pebble.sendAppMessage(message, function () {
      index++;
      sendNext();
    }, function (err) {
      console.log('Failed to send metric: ' + JSON.stringify(err));
    });
  }

  sendNext();
}

function processRunMetrics(data) {
  var run = data.project.run;
  var metrics = [];

  var summary = JSON.parse(run.summaryMetrics);
  var historyRows = run.sampledHistory;

  var keys = Object.keys(summary);
  for (var i = 0; i < keys.length; i++) {
    var key = keys[i];

    if (key.charAt(0) === '_') continue;

    var value = summary[key];

    if (typeof value !== 'number') continue;

    var displayValue;
    if (Math.abs(value) >= 1000) {
      displayValue = value.toFixed(0);
    } else if (Math.abs(value) >= 1) {
      displayValue = value.toFixed(2);
    } else {
      displayValue = value.toFixed(4);
    }

    var history = [];
    for (var j = 0; j < historyRows.length; j++) {
      var row = historyRows[j];
      if (typeof row === 'string') {
        row = JSON.parse(row);
      }
      if (row[key] !== undefined && row[key] !== null) {
        history.push(toFixedPoint(row[key]));
      }
    }

    metrics.push({
      name: key,
      value: displayValue,
      history: history
    });
  }

  var priorities = ['loss', 'accuracy'];

  function getPriority(name) {
    var lower = name.toLowerCase();
    for (var i = 0; i < priorities.length; i++) {
      if (lower.indexOf(priorities[i]) !== -1) return i;
    }
    return priorities.length;
  }

  metrics.sort(function(a, b) {
    var aPriority = getPriority(a.name);
    var bPriority = getPriority(b.name);
    if (aPriority !== bPriority) return aPriority - bPriority;

    var aHasHistory = a.history.length > 0 ? 0 : 1;
    var bHasHistory = b.history.length > 0 ? 0 : 1;
    if (aHasHistory !== bHasHistory) return aHasHistory - bHasHistory;

    return a.name < b.name ? -1 : 1;
  });

  return metrics;
}

Pebble.addEventListener('ready', function () {
  console.log('PebbleKit JS ready');

  var settings = localStorage.getItem('clay-settings');
  var config = settings ? JSON.parse(settings) : {};

  if (!config.apiKey) {
    console.log('No API key configured');
    return;
  }

  cachedClient = new WandbClient(config.apiKey, config.baseUrl);
  cachedClient.fetchAllRuns(function (err, runs) {
    if (err) {
      console.log('Error fetching runs: ' + JSON.stringify(err));
      return;
    }

    console.log('Fetched ' + runs.length + ' runs');
    cachedRuns = runs;
    sendRunsToWatch(runs);
  });
});

Pebble.addEventListener('appmessage', function (e) {
  var runIndex = e.payload['FETCH_RUN_INDEX'];
  if (runIndex !== undefined && runIndex !== null) {
    var runInfo = cachedRuns[runIndex];
    cachedClient.fetchRunMetrics(runInfo.entity, runInfo.project, runInfo.run.name, function (err, data) {
      if (err) {
        console.log('Error fetching metrics: ' + JSON.stringify(err));
        return;
      }

      var metrics = processRunMetrics(data);
      sendMetricsToWatch(metrics);
    });
  }
});
