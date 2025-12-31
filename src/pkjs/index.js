var Clay = require('pebble-clay');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig);

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

function sendRunsToWatch(runs) {
  // Sort: running first, then alphabetically by state
  runs.sort(function (a, b) {
    var stateA = a.run.state || '';
    var stateB = b.run.state || '';
    if (stateA === 'running' && stateB !== 'running') return -1;
    if (stateA !== 'running' && stateB === 'running') return 1;
    if (stateA < stateB) return -1;
    if (stateA > stateB) return 1;
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

Pebble.addEventListener('ready', function () {
  console.log('PebbleKit JS ready');

  var settings = localStorage.getItem('clay-settings');
  var config = settings ? JSON.parse(settings) : {};

  if (!config.apiKey) {
    console.log('No API key configured');
    return;
  }

  var client = new WandbClient(config.apiKey, config.baseUrl);
  client.fetchAllRuns(function (err, runs) {
    if (err) {
      console.log('Error: ' + JSON.stringify(err));
      return;
    }

    console.log('Fetched ' + runs.length + ' runs');
    sendRunsToWatch(runs);
  });
});
