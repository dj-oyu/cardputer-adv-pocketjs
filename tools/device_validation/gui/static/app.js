/* Local-only UI: selections never authorize device operations by themselves. */
(() => {
  'use strict';

  const fragment = new URLSearchParams(window.location.hash.slice(1));
  const sessionToken = fragment.get('token') || '';
  if (window.location.hash) {
    window.history.replaceState(null, '', window.location.pathname + window.location.search);
  }

  const $ = (id) => document.getElementById(id);
  const ACTIVE = new Set(['queued', 'running']);
  const STATUS = { queued: '待機中', running: '実行中', succeeded: '完了', failed: '失敗', cancelled: '中止' };
  const KIND = { scan: '候補の探索', discovery: '候補の探索', plan: 'プラン作成', flash: 'アプリ書き込み', test: '実機テスト', ports: 'ポート取得', 'import-run': 'run の追加', 'import-app': '復旧用アプリの追加' };
  const ACK_LABELS = {
    'replace-app': '選択したポートの機体にある現在のアプリが、このプランの統合ビルドに置き換わることを理解しました。自動復旧は行われません。',
    'device-free': 'この機体を自分が管理しており、他のシリアルモニター・テスト・書き込み処理をすべて停止しました。ポートが他の作業で使用されていないことを確認しました。',
    'manual-download': '機体を手動でダウンロードモードにしました。書き込み後は自分で再起動します。',
    'compatible-layout': '機体の既存 bootloader と partition layout が ESP32-S3 の factory app（開始 0x10000、サイズ 0x300000）に対応していることを確認しました。このツールは既存 partition table を読み出し検証しないことを理解しました。',
    'recovery-unverified': '復旧用ファイルは未検証の候補です。正常に復旧できる保証がなく、検証済みの rollback 手段を確保したことにはならないと理解しました。',
    'recovery-known-good': '選択した復旧用ファイルと同一のバイナリで、この機体が正常起動することを自分で確認済みです。ファイル名や更新日時からの推測ではありません。',
    'manual-reboot-home': 'このプランの SHA-256 に一致する統合ビルドをこの機体に書き込み、手動で通常再起動し、ホーム画面に戻ったことを確認しました。'
  };
  const state = {
    snapshot: null, jobs: new Map(), selectedJob: null, recoveryId: '', plan: null,
    pendingPlan: null, generation: 0, busy: false, refreshing: false,
    connected: false, authFailed: false, executeJob: null, timer: null,
    renderedRuns: '', renderedCandidates: '', renderedPorts: '', renderedJobs: ''
  };

  function node(tag, className, text) {
    const element = document.createElement(tag);
    if (className) element.className = className;
    if (text !== undefined && text !== null) element.textContent = String(text);
    return element;
  }
  function showError(message) {
    $('error-banner').textContent = String(message);
    $('error-banner').hidden = false;
  }
  function clearError() { $('error-banner').hidden = true; }
  function errorText(error) {
    if (typeof error === 'string') return error;
    if (error && typeof error.message === 'string') return error.message;
    return error ? JSON.stringify(error) : '詳細不明のエラー';
  }
  function stamp(value) {
    if (!value) return '日時不明';
    const date = new Date(value);
    return Number.isNaN(date.getTime()) ? String(value) : date.toLocaleString('ja-JP', { hour12: false });
  }
  function bytes(value) {
    const count = Number(value);
    if (!Number.isFinite(count)) return 'サイズ不明';
    return count.toLocaleString('ja-JP') + ' B' + (count >= 1048576 ? ' / ' + (count / 1048576).toFixed(2) + ' MiB' : '');
  }
  function isExecuting() {
    if (Array.from(state.jobs.values()).some((job) => ['flash', 'test', 'execute'].includes(job.kind) && ACTIVE.has(job.status))) return true;
    if (!state.executeJob) return false;
    const job = state.jobs.get(state.executeJob);
    return !job || ACTIVE.has(job.status);
  }
  function selectedAction() {
    return document.querySelector('input[name="action"]:checked').value;
  }
  function selections() {
    return {
      run_id: $('run-select').value,
      recovery_id: state.recoveryId,
      port: $('port-select').value,
      confidence: $('recovery-confidence').value,
      action: selectedAction(),
      cycles: selectedAction() === 'test' ? Number($('test-cycles').value) : 1,
      grid: selectedAction() === 'test' && $('test-grid').checked
    };
  }
  function selectionValid() {
    const input = selections();
    return !!input.run_id && (input.action !== 'flash' || !!input.recovery_id) && !!input.port &&
      Number.isInteger(input.cycles) && input.cycles >= 1 && input.cycles <= 20;
  }
  function idfReady() { return !!(state.snapshot && state.snapshot.idf && state.snapshot.idf.ready); }
  function clearPlan() {
    state.generation += 1;
    state.plan = null;
    state.pendingPlan = null;
    $('plan-section').hidden = true;
    $('acknowledgements').replaceChildren(node('legend', '', '内容を確認し、すべてにチェックしてください'));
    updateControls();
  }

  async function api(path, body) {
    if (!sessionToken || state.authFailed) throw new Error('認証用リンクが必要です。起動したターミナルに表示される、このセッションの URL を開き直してください。');
    const abort = new AbortController();
    const timeout = window.setTimeout(() => abort.abort(), 20000);
    try {
      const options = {
        method: body === undefined ? 'GET' : 'POST',
        credentials: 'omit', cache: 'no-store', redirect: 'error',
        headers: { 'X-Session-Token': sessionToken, Accept: 'application/json' },
        signal: abort.signal
      };
      if (body !== undefined) {
        options.headers['Content-Type'] = 'application/json';
        options.body = JSON.stringify(body);
      }
      const response = await fetch(path, options);
      let data;
      try { data = await response.json(); }
      catch (_) { throw new Error('サーバーの応答を読み取れませんでした。起動したターミナルの表示を確認してください。'); }
      if (!response.ok) {
        if (response.status === 401 || response.status === 403) state.authFailed = true;
        throw new Error(errorText(data.detail || data.error || data.message || 'HTTP ' + response.status));
      }
      return data;
    } catch (error) {
      if (error.name === 'AbortError') throw new Error('応答がタイムアウトしました。処理が受け付けられた可能性があるため、再実行せず「状態を更新」で実行状況を確認してください。');
      throw error;
    } finally { window.clearTimeout(timeout); }
  }

  function setSelectOptions(element, entries, placeholder, labelFor) {
    const previous = element.value;
    element.replaceChildren(node('option', '', placeholder));
    element.firstElementChild.value = '';
    for (const item of entries) {
      const option = node('option', '', labelFor(item));
      option.value = String(item.id === undefined ? item.device : item.id);
      element.append(option);
    }
    if (Array.from(element.options).some((option) => option.value === previous)) element.value = previous;
    if (previous && element.value !== previous) clearPlan();
  }

  function detailRow(container, label, value) {
    const row = node('div', 'detail-row');
    row.append(node('span', 'detail-label', label), node('span', 'mono', value || '—'));
    container.append(row);
  }
  function renderRunDetails() {
    const runs = state.snapshot ? state.snapshot.runs || [] : [];
    const run = runs.find((item) => String(item.id) === $('run-select').value);
    const panel = $('run-details');
    panel.hidden = !run;
    panel.replaceChildren();
    if (!run) return;
    detailRow(panel, 'RUN', run.path);
    detailRow(panel, 'COMMIT', run.commit);
    detailRow(panel, 'STATUS', run.status);
  }

  function renderCandidates(candidates) {
    const signature = JSON.stringify(candidates);
    if (signature === state.renderedCandidates) return;
    const previous = state.renderedCandidates ? JSON.parse(state.renderedCandidates) : [];
    const previousSelection = previous.find((item) => String(item.id) === state.recoveryId);
    state.renderedCandidates = signature;
    const selectable = candidates.filter((item) => item.kind === 'app-candidate');
    const excluded = candidates.filter((item) => item.kind !== 'app-candidate');
    const selected = selectable.find((item) => String(item.id) === state.recoveryId);
    // The same candidate ID can refer to a file whose metadata has changed.
    // Never preserve a reviewed plan or a known-good attestation across that change.
    if (state.recoveryId && (!selected || JSON.stringify(previousSelection) !== JSON.stringify(selected))) {
      if (!selected) state.recoveryId = '';
      $('recovery-confidence').value = 'unverified-candidate';
      clearPlan();
    }
    const list = $('recovery-candidates');
    list.replaceChildren(node('legend', 'sr-only', '復旧用アプリの候補'));
    if (!selectable.length) list.append(node('p', 'empty-state', '選択できる app 候補はありません。「候補を探す」か、下の絶対パス入力でファイルを追加してください。'));
    for (const candidate of selectable) {
      const label = node('label', 'candidate');
      const radio = node('input');
      radio.type = 'radio'; radio.name = 'recovery'; radio.value = String(candidate.id);
      radio.checked = state.recoveryId === String(candidate.id);
      radio.addEventListener('change', () => {
        state.recoveryId = radio.value;
        $('recovery-confidence').value = 'unverified-candidate';
        clearPlan();
      });
      const body = node('span', 'candidate-body');
      body.append(node('span', 'candidate-name', candidate.name || 'app candidate'));
      body.append(node('span', 'candidate-path', candidate.path));
      const metadata = node('span', 'candidate-meta');
      metadata.append(node('span', '', bytes(candidate.bytes)), node('span', '', stamp(candidate.modifiedUtc)));
      if (candidate.source === 'manual') metadata.append(node('span', 'tag', '手動追加'));
      if (candidate.worktree) metadata.append(node('span', '', 'worktree: ' + candidate.worktree));
      body.append(metadata);
      label.append(radio, body);
      list.append(label);
    }
    $('excluded-section').hidden = excluded.length === 0;
    $('excluded-summary').textContent = '選択できないファイル（' + excluded.length + '）';
    $('excluded-files').replaceChildren();
    for (const candidate of excluded) {
      const item = node('div', 'excluded-item');
      item.append(node('strong', '', candidate.name || candidate.path), node('div', 'mono', candidate.path));
      if (candidate.source === 'manual') item.append(node('span', 'tag', '手動追加'));
      item.append(node('div', '', (candidate.reason || 'app 候補として認められないファイル') + ' · ' + bytes(candidate.bytes) + ' · ' + stamp(candidate.modifiedUtc)));
      $('excluded-files').append(item);
    }
  }

  function renderSnapshot(snapshot) {
    state.snapshot = snapshot;
    $('project-path').textContent = typeof snapshot.project === 'string' ? snapshot.project : JSON.stringify(snapshot.project || '—');
    const idf = snapshot.idf || {};
    $('idf-status').textContent = idf.ready ? '準備 OK' : '環境の準備が必要';
    $('idf-status').title = [idf.idf_path, idf.python].filter(Boolean).join('\n');
    $('idf-error').hidden = !!idf.ready;
    $('idf-error').textContent = '実行環境を確認してください: ' + errorText(idf.error || 'Windows の EIM ESP-IDF v6.0.1 と、その Python 環境が必要です。対応環境のターミナルから起動し直してください。') + ' ファイルの一覧は確認できますが、実機操作は開始できません。';
    const runs = snapshot.runs || [];
    const runSignature = JSON.stringify(runs);
    if (runSignature !== state.renderedRuns) {
      state.renderedRuns = runSignature;
      setSelectOptions($('run-select'), runs, runs.length ? 'run を選択してください' : 'run がありません。保持済みフォルダーを追加してください', (run) => (run.path || run.id) + ' · ' + (run.status || '状態不明'));
      renderRunDetails();
    }
    const ports = snapshot.ports || [];
    const portSignature = JSON.stringify(ports);
    if (portSignature !== state.renderedPorts) {
      state.renderedPorts = portSignature;
      setSelectOptions($('port-select'), ports, ports.length ? 'ポートを選択してください' : 'ポートがありません。「再取得」を押してください', (port) => port.device + (port.description ? ' · ' + port.description : ''));
    }
    renderCandidates(snapshot.candidates || []);
    const discovery = snapshot.discovery || {};
    const scanning = ACTIVE.has(discovery.status) || discovery.status === 'scanning';
    $('discovery-status').textContent = scanning ? '候補を探索しています…' : ({ idle: '探索はユーザー操作で開始します', succeeded: '探索が完了しました', complete: '探索が完了しました', completed: '探索が完了しました', failed: '探索に失敗しました', cancelled: '探索を中止しました' }[discovery.status] || discovery.status || '候補の探索を開始できます');
    $('cancel-scan').hidden = !scanning;
    $('scan-candidates').dataset.scanning = scanning ? 'true' : 'false';
    $('discovery-warning').hidden = !discovery.warning;
    $('discovery-warning').textContent = discovery.warning || '';
    const snapshotJobs = snapshot.jobs || [];
    const liveIds = new Set(snapshotJobs.map((job) => job.id));
    // Completed jobs can be evicted by the bounded server history. Do not keep polling them.
    for (const [id, job] of state.jobs) {
      if (!liveIds.has(id) && !ACTIVE.has(job.status)) {
        state.jobs.delete(id);
        if (state.executeJob === id) state.executeJob = null;
        if (state.selectedJob === id) state.selectedJob = null;
      }
    }
    for (const job of snapshotJobs) rememberJob(job);
    renderJobs();
    updateControls();
  }

  function rememberJob(job) {
    if (!job || !job.id) return;
    const existing = state.jobs.get(job.id) || {};
    state.jobs.set(job.id, Object.assign({}, existing, job));
    if (!state.selectedJob) state.selectedJob = job.id;
    if (state.pendingPlan && job.id === state.pendingPlan.id && !ACTIVE.has(job.status)) {
      const pending = state.pendingPlan;
      state.pendingPlan = null;
      if (job.status === 'succeeded' && job.result && job.result.plan && pending.generation === state.generation && pending.selection === JSON.stringify(selections())) {
        renderPlan(job.result.plan, job.result.plan.warnings || job.result.warnings || []);
      } else if (job.status === 'failed') {
        showError('プランを作成できませんでした: ' + errorText(job.error || '実行ログを確認してください。'));
      }
    }
  }

  function renderJobs() {
    const jobs = Array.from(state.jobs.values()).reverse().slice(0, 12);
    const signature = JSON.stringify(jobs.map((job) => [job.id, job.kind, job.status])) + state.selectedJob;
    $('job-count').textContent = String(state.jobs.size);
    if (signature !== state.renderedJobs) {
      state.renderedJobs = signature;
      const list = $('job-list'); list.replaceChildren();
      if (!jobs.length) list.append(node('p', 'empty-state', 'まだ実行はありません。'));
      for (const job of jobs) {
        const button = node('button', 'job-button');
        button.type = 'button'; button.setAttribute('aria-pressed', String(state.selectedJob === job.id));
        const name = node('span', 'job-button-name', KIND[job.kind] || job.kind || '処理');
        name.append(node('span', 'job-button-id', String(job.id).slice(0, 14)));
        button.append(name, node('span', 'tag ' + (job.status === 'failed' ? 'danger' : job.status === 'succeeded' ? 'success' : ''), STATUS[job.status] || job.status));
        button.addEventListener('click', () => { state.selectedJob = job.id; renderJobs(); refreshJob(job.id).catch(showRefreshError); });
        list.append(button);
      }
    }
    const selected = state.jobs.get(state.selectedJob);
    $('job-detail').hidden = !selected;
    if (!selected) return;
    $('log-title').textContent = KIND[selected.kind] || selected.kind || 'ログ';
    $('log-status').textContent = STATUS[selected.status] || selected.status;
    const log = $('job-log');
    const follow = log.scrollHeight - log.scrollTop - log.clientHeight < 40;
    const lines = Array.isArray(selected.log) ? selected.log : [selected.log || ''];
    const logText = lines.slice(-300).map((line) => typeof line === 'string' ? line : JSON.stringify(line)).join('\n');
    if (log.textContent !== logText) {
      log.textContent = logText || (ACTIVE.has(selected.status) ? 'ログを待っています…' : 'ログはありません。');
      if (follow) log.scrollTop = log.scrollHeight;
    }
    $('job-error').hidden = !selected.error;
    $('job-error').textContent = selected.error ? errorText(selected.error) : '';
    const result = selected.result;
    const resultPanel = $('job-result');
    resultPanel.hidden = !result || !!result.plan;
    if (result && !result.plan) {
      let summary = JSON.stringify(result, null, 2);
      if ((selected.kind === 'flash' || result.action === 'flash') && selected.status === 'succeeded') summary = '書き込みが完了しました。機体を手動で通常再起動し、ホーム画面を確認してから、別途テストのプランを作成してください。\n\n' + summary;
      if ((selected.kind === 'test' || result.action === 'test') && selected.status === 'succeeded') summary = '自動テストのコマンドが完了しました。LCD・物理キー・音声は実機で別途確認してください。ログの成功マーカーだけで、すべての動作が正常とは判断できません。\n\n' + summary;
      resultPanel.textContent = summary.length > 12000 ? summary.slice(0, 12000) + '\n（表示を省略）' : summary;
    }
  }

  function addPlanDetail(label, value) {
    $('plan-details').append(node('dt', '', label), node('dd', '', value === undefined || value === null ? '—' : String(value)));
  }
  function renderPlan(plan, warnings) {
    if (!plan.id || !plan.digest || !Array.isArray(plan.required_ack) || !plan.required_ack.length) {
      showError('確認項目を含む有効なプランが返されませんでした。実行はできません。');
      return;
    }
    state.plan = plan;
    const unknown = plan.required_ack.filter((key) => !Object.prototype.hasOwnProperty.call(ACK_LABELS, key));
    state.plan.unknownAcknowledgements = unknown.length > 0;
    $('plan-section').hidden = false;
    $('plan-action-tag').textContent = plan.action === 'flash' ? 'APP FLASH' : 'DEVICE TEST';
    $('plan-action-tag').className = 'tag ' + (plan.action === 'flash' ? 'amber' : '');
    $('plan-details').replaceChildren();
    addPlanDetail('操作 / 接続先', (plan.action === 'flash' ? 'アプリ領域への書き込み' : '実機テスト') + ' / ' + plan.port);
    if (plan.portIdentity) addPlanDetail('接続先の識別情報', [plan.portIdentity.description, plan.portIdentity.hwid].filter(Boolean).join(' / ') || JSON.stringify(plan.portIdentity));
    addPlanDetail('対象 run', plan.runRoot);
    addPlanDetail('対象 commit（完全 SHA）', plan.commit);
    addPlanDetail('書き込み / 検証対象バイナリ', plan.binary);
    addPlanDetail('対象バイナリ SHA-256', plan.binarySha256);
    addPlanDetail('manifest SHA-256', plan.manifestSha256);
    if (plan.action === 'flash') {
      addPlanDetail('復旧用アプリ', plan.recoveryImage);
      addPlanDetail('復旧用アプリ SHA-256', plan.recoverySha256);
      addPlanDetail('復旧用アプリの確認状況', plan.recoveryConfidence === 'user-attested-known-good' ? 'ユーザーが、この正確なファイルでの正常起動を確認済みと申告' : '未検証の候補 · 復旧できることは保証されません');
      addPlanDetail('app 領域 / 最大サイズ', (plan.region || '—') + ' / ' + bytes(plan.maxBytes));
    }
    $('flash-warning').hidden = plan.action !== 'flash';
    if (plan.action === 'test') addPlanDetail('テスト設定', '繰り返し ' + (plan.cycles === undefined ? selections().cycles : plan.cycles) + ' 回 / grid ' + ((plan.grid === undefined ? selections().grid : plan.grid) ? 'あり' : 'なし'));
    addPlanDetail('プラン digest', plan.digest);
    $('plan-command').textContent = Array.isArray(plan.command) ? plan.command.map((argument) => JSON.stringify(String(argument))).join(' ') : String(plan.command || '—');
    const warningsPanel = $('plan-warnings'); warningsPanel.replaceChildren();
    for (const warning of warnings) warningsPanel.append(node('div', 'notice warning compact', errorText(warning)));
    if (unknown.length) warningsPanel.append(node('div', 'notice danger compact', '未対応の確認項目があるため実行できません: ' + unknown.join(', ') + '。GUI とサーバーを同じバージョンで起動してください。'));
    const acknowledgements = $('acknowledgements');
    acknowledgements.replaceChildren(node('legend', '', '内容を確認し、すべてにチェックしてください'));
    for (const key of plan.required_ack) {
      const label = node('label', 'ack-row');
      const checkbox = node('input'); checkbox.type = 'checkbox'; checkbox.value = key;
      checkbox.addEventListener('change', updateControls);
      label.append(checkbox, node('span', '', ACK_LABELS[key] || '未対応の確認項目: ' + key));
      acknowledgements.append(label);
    }
    updateControls();
    $('plan-title').focus({ preventScroll: true });
    $('plan-section').scrollIntoView({ behavior: 'smooth', block: 'start' });
  }

  function updateControls() {
    const unavailable = !sessionToken || state.authFailed || !state.connected;
    const locked = state.busy || isExecuting();
    const planPending = !!state.pendingPlan;
    $('build-plan').disabled = unavailable || locked || planPending || !idfReady() || !selectionValid();
    $('build-plan').textContent = planPending ? 'プランを検証しています…' : '実行プランを確認 →';
    $('refresh-state').disabled = !sessionToken || state.authFailed || state.refreshing;
    $('scan-candidates').disabled = unavailable || locked || $('scan-candidates').dataset.scanning === 'true';
    $('cancel-scan').disabled = unavailable || state.busy;
    $('refresh-ports').disabled = unavailable || locked;
    $('import-run').disabled = unavailable || locked || !$('import-path').value.trim();
    $('import-path').disabled = locked;
    $('import-app').disabled = unavailable || locked || !$('import-app-path').value.trim();
    $('import-app-path').disabled = locked;
    for (const element of document.querySelectorAll('.plan-input, input[name="recovery"]')) element.disabled = locked;
    $('test-options').hidden = selectedAction() !== 'test';
    $('discard-plan').disabled = locked;
    const input = selections();
    let missing = [];
    if (!input.run_id) missing.push('対象 run');
    if (input.action === 'flash' && !input.recovery_id) missing.push('復旧用ファイル');
    if (!input.port) missing.push('ポート');
    if (!Number.isInteger(input.cycles) || input.cycles < 1 || input.cycles > 20) missing.push('1–20 の繰り返し回数');
    $('selection-summary').textContent = missing.length ? missing.join('・') + 'を指定してください。' : !idfReady() ? 'ESP-IDF 環境の問題を解消するとプランを作成できます。' : '選択が揃いました。SHA-256 と実行内容を確認してください。';
    let expired = true;
    if (state.plan) {
      const deadline = new Date(state.plan.expiresUtc).getTime();
      expired = !Number.isFinite(deadline) || deadline <= Date.now();
      $('plan-expires').textContent = expired ? 'このプランは期限切れです。新しくプランを作成してください。' : '有効期限: ' + stamp(state.plan.expiresUtc) + ' · 入力を変更すると無効になります';
      const checks = Array.from($('acknowledgements').querySelectorAll('input[type="checkbox"]'));
      $('execute-plan').disabled = unavailable || locked || expired || state.plan.unknownAcknowledgements || !checks.length || !checks.every((box) => box.checked);
      $('execute-plan').textContent = state.plan.action === 'flash' ? '確認してアプリを書き込む' : '確認してテストを開始';
      $('execute-plan').className = 'button destructive' + (state.plan.action === 'test' ? ' test-action' : '');
      for (const check of checks) check.disabled = locked || expired;
    } else $('execute-plan').disabled = true;
  }

  function showRefreshError(error) {
    state.connected = false;
    $('connection-status').hidden = false;
    $('connection-status').className = 'notice danger';
    $('connection-status').textContent = state.authFailed ? '認証が切れています。ターミナルに表示された起動 URL を開き直してください。' : 'サーバーと通信できません: ' + errorText(error) + ' 起動したターミナルを確認してください。';
    updateControls();
  }
  async function refreshJob(id) {
    const job = await api('/api/jobs/' + encodeURIComponent(id));
    rememberJob(job);
    renderJobs(); updateControls();
  }
  async function refreshState() {
    if (state.refreshing || !sessionToken || state.authFailed) return;
    state.refreshing = true;
    updateControls();
    try {
      const snapshot = await api('/api/state');
      state.connected = true;
      $('connection-status').hidden = true;
      renderSnapshot(snapshot);
      const needed = new Set();
      if (state.selectedJob) needed.add(state.selectedJob);
      if (state.pendingPlan) needed.add(state.pendingPlan.id);
      if (state.executeJob && isExecuting()) needed.add(state.executeJob);
      for (const id of needed) await refreshJob(id);
    } catch (error) { showRefreshError(error); }
    finally { state.refreshing = false; updateControls(); scheduleRefresh(); }
  }
  function scheduleRefresh() {
    window.clearTimeout(state.timer);
    if (!sessionToken || state.authFailed) return;
    const active = Array.from(state.jobs.values()).some((job) => ACTIVE.has(job.status));
    const discovery = state.snapshot && state.snapshot.discovery;
    const scanning = discovery && (ACTIVE.has(discovery.status) || discovery.status === 'scanning');
    state.timer = window.setTimeout(refreshState, active || scanning || state.pendingPlan ? 1000 : 8000);
  }
  async function mutate(path, body, onSuccess) {
    if (state.busy || isExecuting()) return;
    state.busy = true; clearError(); updateControls();
    try {
      const result = await api(path, body);
      if (onSuccess) onSuccess(result);
      if (result.job_id) {
        state.selectedJob = result.job_id;
        if (!state.jobs.has(result.job_id)) rememberJob({ id: result.job_id, kind: path.split('/').pop(), status: 'queued', log: [] });
      }
    } catch (error) { showError(errorText(error)); }
    finally { state.busy = false; updateControls(); await refreshState(); }
  }

  for (const input of document.querySelectorAll('.plan-input')) {
    input.addEventListener('change', () => { clearPlan(); if (input.id === 'run-select') renderRunDetails(); });
  }
  $('test-cycles').addEventListener('input', clearPlan);
  $('import-path').addEventListener('input', updateControls);
  $('import-app-path').addEventListener('input', updateControls);
  $('refresh-state').addEventListener('click', () => { clearError(); refreshState(); });
  $('scan-candidates').addEventListener('click', () => { clearPlan(); mutate('/api/scan', {}); });
  $('cancel-scan').addEventListener('click', () => mutate('/api/cancel-scan', {}));
  $('refresh-ports').addEventListener('click', () => { clearPlan(); mutate('/api/ports', {}); });
  $('import-run').addEventListener('click', () => {
    const path = $('import-path').value.trim();
    if (!path) return;
    clearPlan();
    mutate('/api/import-run', { path }, () => { $('import-path').value = ''; });
  });
  $('import-app').addEventListener('click', () => {
    const path = $('import-app-path').value.trim();
    if (!path) return;
    clearPlan();
    mutate('/api/import-app', { path }, () => { $('import-app-path').value = ''; });
  });
  $('build-plan').addEventListener('click', () => {
    if (!selectionValid() || !idfReady()) return;
    clearPlan();
    const input = selections();
    const generation = state.generation;
    mutate('/api/plan', input, (result) => {
      if (!result.job_id) throw new Error('プラン作成の job ID が返されませんでした。実行状況を確認してください。');
      state.pendingPlan = { id: result.job_id, generation, selection: JSON.stringify(input) };
    });
  });
  $('discard-plan').addEventListener('click', clearPlan);
  $('execute-plan').addEventListener('click', () => {
    updateControls();
    if ($('execute-plan').disabled || !state.plan) return;
    const plan = state.plan;
    const acknowledgements = Array.from($('acknowledgements').querySelectorAll('input:checked')).map((box) => box.value);
    // Consume the visible plan before sending. A failed/uncertain request never re-enables it.
    clearPlan();
    mutate('/api/execute', { plan_id: plan.id, plan_digest: plan.digest, acknowledgements }, (result) => {
      if (result.job_id) {
        state.executeJob = result.job_id;
        rememberJob({ id: result.job_id, kind: plan.action, status: 'queued', log: [] });
      }
    });
  });

  window.setInterval(() => { if (state.plan) updateControls(); }, 1000);
  if (!sessionToken) {
    $('connection-status').className = 'notice warning';
    $('connection-status').textContent = '認証用リンクがありません。GUI を起動したターミナルに表示される #token=… 付きの URL を開いてください。更新後も、元の起動 URL から開き直せます。トークンはブラウザーに保存しません。';
    $('idf-status').textContent = '未接続';
    updateControls();
  } else refreshState();
})();
