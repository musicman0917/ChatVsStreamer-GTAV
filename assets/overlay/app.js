(function () {
  'use strict';

  var catalogEl = document.getElementById('catalog');
  var leaderboardListEl = document.getElementById('leaderboard-list');
  var chatListEl = document.getElementById('chat-list');
  var alertsEl = document.getElementById('alerts');

  var MAX_CHAT_LINES = 40;
  var ALERT_LIFETIME_MS = 5000;

  function connect() {
    var proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    var ws = new WebSocket(proto + '//' + location.host + '/ws');

    ws.onmessage = function (event) {
      try {
        handleMessage(JSON.parse(event.data));
      } catch (err) {
        console.error('Bad overlay message', err, event.data);
      }
    };

    ws.onclose = function () {
      setTimeout(connect, 2000); // OBS keeps the browser source alive across mod restarts; just keep retrying.
    };

    ws.onerror = function () {
      ws.close();
    };
  }

  function handleMessage(msg) {
    switch (msg.type) {
      case 'snapshot':
        renderCatalog(msg.catalog || []);
        renderLeaderboard(msg.leaderboard || []);
        (msg.recentChat || []).slice().reverse().forEach(appendChatLine);
        break;
      case 'shop_catalog':
        renderCatalog(msg.commands || []);
        break;
      case 'leaderboard':
        renderLeaderboard(msg.entries || []);
        break;
      case 'chat_message':
        appendChatLine(msg);
        break;
      case 'purchase':
        showAlert(msg.success
          ? (msg.viewerLogin + ' bought ' + msg.displayName + ' (' + msg.cost + ' pts)')
          : (msg.viewerLogin + ' failed to buy ' + msg.displayName), 'purchase');
        break;
      case 'effect_fired':
        showAlert((msg.forcedByHotkey ? 'HOTKEY: ' : msg.viewerLogin + ' triggered ') + msg.displayName, 'fired');
        break;
      case 'refund':
        showAlert(msg.viewerLogin + ' refunded ' + msg.amount + ' pts (' + msg.reason + ')', 'refund');
        break;
      case 'balance_update':
        // Leaderboard refresh is driven by explicit 'leaderboard' broadcasts, not every balance tick.
        break;
      case 'clip_created':
        showAlert('Clip saved!', 'clip');
        break;
      default:
        break;
    }
  }

  function renderCatalog(commands) {
    catalogEl.innerHTML = '';
    var groups = {};
    commands.forEach(function (cmd) {
      (groups[cmd.category] = groups[cmd.category] || []).push(cmd);
    });

    Object.keys(groups).forEach(function (category) {
      var title = document.createElement('div');
      title.className = 'catalog-group-title';
      title.textContent = category;
      catalogEl.appendChild(title);

      groups[category].forEach(function (cmd) {
        var row = document.createElement('div');
        row.className = 'catalog-item tier-' + cmd.tier + (cmd.enabled ? '' : ' disabled');
        row.innerHTML =
          '<span class="cmd">' + escapeHtml(cmd.command) + '</span>' +
          '<span class="cost">' + cmd.cost + '</span>';
        catalogEl.appendChild(row);
      });
    });
  }

  function renderLeaderboard(entries) {
    leaderboardListEl.innerHTML = '';
    entries.forEach(function (entry) {
      var li = document.createElement('li');
      li.innerHTML = escapeHtml(entry.viewerLogin) + '<span class="balance">' + entry.balance + '</span>';
      leaderboardListEl.appendChild(li);
    });
  }

  function appendChatLine(msg) {
    var line = document.createElement('div');
    line.className = 'chat-line' + (msg.isSub ? ' sub' : '') + (msg.isMod ? ' mod' : '');
    line.innerHTML = '<span class="name">' + escapeHtml(msg.displayName || msg.viewerLogin || '') + '</span>' + escapeHtml(msg.message || '');
    chatListEl.insertBefore(line, chatListEl.firstChild);

    while (chatListEl.children.length > MAX_CHAT_LINES) {
      chatListEl.removeChild(chatListEl.lastChild);
    }
  }

  function showAlert(text, kind) {
    var el = document.createElement('div');
    el.className = 'alert ' + kind;
    el.textContent = text;
    alertsEl.appendChild(el);
    setTimeout(function () { el.remove(); }, ALERT_LIFETIME_MS);
  }

  function escapeHtml(str) {
    return String(str)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }

  connect();
})();
