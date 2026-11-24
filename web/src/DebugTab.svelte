<!-- SPDX-License-Identifier: MIT -->
<script>
  import { onMount } from 'svelte';

  let { onShowDumps } = $props();

  const debugHeader = { 'X-Dumper-Debug': '1' };
  const sources = [
    { id: 'metrics', label: 'Metrics', kind: 'ampr' },
    { id: 'fsfail', label: 'FS Fail', kind: 'ampr' },
    { id: 'modules', label: 'Modules', kind: 'ampr' },
    { id: 'threads', label: 'Threads', kind: 'ampr' },
    { id: 'queues', label: 'Queues', kind: 'ampr' },
    { id: 'set', label: 'Knobs', kind: 'ampr' },
    { id: 'trace', label: 'Trace', kind: 'file' },
    { id: 'tune', label: 'Tune file', kind: 'file' },
  ];

  let status = $state(null);
  let loading = $state(true);
  let error = $state('');
  let selected = $state('metrics');
  let logText = $state('');
  let logLoading = $state(false);
  let logSize = $state(0);
  let logPane = $state();
  let crashNote = $state('');
  let capturedDump = $state('');
  let busy = $state('');

  let amprRunning = $derived(Boolean(status?.ampr?.reachable));
  const apiSources = sources.filter((source) => source.kind === 'ampr');
  const fileSources = sources.filter((source) => source.kind === 'file');

  async function readError(response) {
    const text = await response.text();
    return `${response.status} ${response.statusText}: ${text.slice(0, 300)}`;
  }

  async function getJson(path) {
    const response = await fetch(path);
    if (!response.ok) {
      throw new Error(await readError(response));
    }
    return response.json();
  }

  async function postJson(path) {
    const response = await fetch(path, { method: 'POST', headers: debugHeader });
    if (!response.ok) {
      throw new Error(await readError(response));
    }
    return response.json();
  }

  async function loadLog(id) {
    const source = sources.find((item) => item.id === id) ?? sources[0];
    selected = source.id;
    logLoading = true;
    logText = 'Loading…';
    try {
      const url =
        source.kind === 'ampr'
          ? `/api/debug/ampr?page=${encodeURIComponent(source.id)}`
          : source.id === 'trace'
            ? '/api/debug/trace'
            : '/api/debug/tune';
      const response = await fetch(url);
      const text = await response.text();
      logText = response.ok ? text : `Error: ${response.status} ${response.statusText}${text ? `: ${text}` : ''}`;
      logSize = text.length;
    } catch (cause) {
      logText = '';
      logSize = 0;
      error = String(cause.message ?? cause);
    }
    logLoading = false;
  }

  async function loadEnvironment() {
    loading = true;
    error = '';
    try {
      status = await getJson('/api/debug/status');
    } catch (cause) {
      error = String(cause.message ?? cause);
    }
    loading = false;
    if (!status?.ampr?.reachable && status?.trace?.exists && selected === 'metrics') {
      selected = 'trace';
    }
    await loadLog(selected);
  }

  export function refresh() {
    return loadEnvironment();
  }

  async function triggerCrash() {
    if (!confirm('Trigger a crash in the running game?\n\nThe coredump will be saved by CoreDump Dumper. Do this only while the game is frozen or the bug is visible.')) {
      return;
    }
    busy = 'crash';
    crashNote = 'Triggering crash…';
    capturedDump = '';
    let before = [];
    try {
      before = (await getJson('/api/dumps')).dumps.map((dump) => dump.name);
      await postJson('/api/debug/crash');
      crashNote = 'Crash triggered. Waiting for the coredump…';
    } catch (cause) {
      busy = '';
      crashNote = '';
      error = String(cause.message ?? cause);
      return;
    }

    for (let attempt = 0; attempt < 30; attempt++) {
      await new Promise((resolve) => setTimeout(resolve, 1500));
      try {
        const dumps = (await getJson('/api/dumps')).dumps;
        const fresh = dumps.find((dump) => !before.includes(dump.name));
        if (fresh) {
          capturedDump = fresh.name;
          crashNote = `Coredump saved: ${fresh.name}.`;
          await loadEnvironment();
          busy = '';
          return;
        }
      } catch {
        // The dumper may be busy writing the new dump; retry.
      }
    }
    crashNote = 'No new coredump appeared. Check the Coredumps tab and the dumper log.';
    busy = '';
  }

  function formatBytes(value) {
    const units = ['B', 'KiB', 'MiB', 'GiB'];
    let size = value;
    let unit = 0;
    while (size >= 1024 && unit < units.length - 1) {
      size /= 1024;
      unit += 1;
    }
    return unit === 0 ? `${size} B` : `${size.toFixed(1)} ${units[unit]}`;
  }

  function formatTime(seconds) {
    return seconds ? new Date(seconds * 1000).toLocaleString() : '–';
  }

  $effect(() => {
    logText;
    if (logPane) {
      // Reports open at the top; the trace is an event log, so open at the tail.
      logPane.scrollTop = selected === 'trace' || selected === 'tune' ? logPane.scrollHeight : 0;
    }
  });

  onMount(() => {
    loadEnvironment();
  });
</script>

<div class="flex flex-col gap-4">
  {#if error}
    <div class="alert alert-error text-sm">
      <span class="min-w-0 break-words">{error}</span>
      <button class="btn btn-ghost btn-xs" onclick={() => (error = '')}>Dismiss</button>
    </div>
  {/if}

  <div class="card border border-base-300 bg-base-100 shadow-sm">
    <div class="card-body gap-4">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div class="flex min-w-0 items-start gap-3">
          <div class="flex size-10 shrink-0 items-center justify-center rounded-box bg-primary/10 text-primary">
            <svg class="size-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true">
              <path d="M3 12h4l2-7 4 14 2-7h6" />
            </svg>
          </div>
          <div class="min-w-0">
            <h2 class="text-lg font-semibold leading-tight">Ampr-emu-ng</h2>
            <div class="mt-1 flex flex-wrap items-center gap-2">
              <span class="badge badge-sm gap-1.5 {amprRunning ? 'badge-success' : 'badge-ghost'}">
                <span class="size-1.5 rounded-full {amprRunning ? 'bg-current' : 'bg-base-content/40'}"></span>
                {amprRunning ? 'Online' : 'Debug build not running'}
              </span>
              {#if amprRunning}
                <span class="text-xs text-base-content/50">debug API :9090</span>
              {/if}
            </div>
          </div>
        </div>
        <div class="flex flex-wrap items-center gap-2">
          <button class="btn btn-ghost btn-sm gap-1.5" onclick={loadEnvironment} disabled={loading}>
            {#if loading}
              <span class="loading loading-spinner loading-xs"></span>
            {:else}
              <svg class="size-4" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true">
                <path d="M20 11a8 8 0 1 0-2.3 5.7" />
                <path d="M20 4v7h-7" />
              </svg>
            {/if}
            Refresh
          </button>
          {#if amprRunning}
            <a class="btn btn-primary btn-sm gap-1.5" href="/api/debug/logs.zip" download="ampr-emu-ng-logs.zip">
              <svg class="size-4" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true">
                <path d="M12 3v12" />
                <path d="m7 10 5 5 5-5" />
                <path d="M5 21h14" />
              </svg>
              Download all logs
            </a>
          {:else}
            <button class="btn btn-primary btn-sm gap-1.5" disabled title="Start the game with the debug build to download all logs">
              <svg class="size-4" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true">
                <path d="M12 3v12" />
                <path d="m7 10 5 5 5-5" />
                <path d="M5 21h14" />
              </svg>
              Download all logs
            </button>
          {/if}
        </div>
      </div>

      <div class="rounded-box bg-base-200/60 p-3">
        <div class="grid gap-3 sm:grid-cols-2">
          <div class="min-w-0">
            <div class="text-[11px] font-medium uppercase tracking-wide text-base-content/50">Build</div>
            <div class="mt-0.5 break-all font-mono text-xs text-base-content/80">
              {amprRunning ? status?.ampr?.build || 'unknown' : '—'}
            </div>
          </div>
          <div class="min-w-0">
            <div class="text-[11px] font-medium uppercase tracking-wide text-base-content/50">Last ampr_emu_trace.txt</div>
            <div class="mt-0.5 flex flex-wrap items-center gap-2">
              <span class="text-sm {status?.trace?.exists ? 'text-base-content/80' : 'text-base-content/50'}">
                {status?.trace?.exists ? formatTime(status.trace.mtime) : 'not present'}
              </span>
              {#if status?.trace?.exists}
                <a class="btn btn-ghost btn-xs gap-1" href="/api/debug/trace" download="ampr_emu_trace.txt">
                  <svg class="size-3.5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true">
                    <path d="M12 3v12" />
                    <path d="m7 10 5 5 5-5" />
                    <path d="M5 21h14" />
                  </svg>
                  Download
                </a>
              {/if}
            </div>
          </div>
        </div>
      </div>

      {#if !amprRunning}
        <p class="flex items-start gap-1.5 text-xs text-base-content/60">
          <svg class="mt-0.5 size-3.5 shrink-0" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true">
            <circle cx="12" cy="12" r="9" />
            <path d="M12 16v-5" />
            <path d="M12 8h.01" />
          </svg>
          Start the game with the debug build to enable the 9090 pages. The trace still works after a crash on start.
        </p>
      {/if}
    </div>
  </div>

  <div class="card border border-base-300 bg-base-100 shadow-sm">
    <div class="card-body gap-2 py-3">
      <div class="flex flex-wrap items-start gap-2">
        <svg class="mt-0.5 size-4 shrink-0 text-warning" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true">
          <path d="M12 9v4" />
          <path d="M12 17h.01" />
          <path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0Z" />
        </svg>
        <div class="min-w-0 flex-1 text-xs">
          <span class="font-medium">Frozen game?</span>
          {#if amprRunning}
            <span class="text-base-content/60">Trigger a crash to create a coredump. The game closes and unsaved progress is lost; download the dump from the Coredumps tab afterwards.</span>
          {:else}
            <span class="text-base-content/60">Start the game with the debug build to enable the crash button. If it already crashed, the trace and coredump are still available.</span>
          {/if}
        </div>
        <button
          class="btn btn-warning btn-outline btn-xs shrink-0"
          onclick={triggerCrash}
          disabled={busy === 'crash' || !amprRunning}
          title={!amprRunning ? 'The 9090 API is not reachable' : undefined}
        >
          {#if busy === 'crash'}<span class="loading loading-spinner loading-xs"></span>{/if}
          Trigger crash
        </button>
        {#if capturedDump && onShowDumps}
          <button class="btn btn-ghost btn-xs shrink-0" onclick={onShowDumps}>View coredumps</button>
        {/if}
      </div>
      {#if crashNote}<p class="text-xs text-base-content/70">{crashNote}</p>{/if}
    </div>
  </div>

  <div class="card border border-base-300 bg-base-100 shadow-sm">
    <div class="card-body gap-3">
      <div class="flex flex-wrap items-start justify-between gap-3">
        <div class="flex flex-col gap-2">
          <div class="flex flex-wrap items-center gap-2">
            <span class="w-24 shrink-0 text-[11px] font-semibold uppercase tracking-wide text-base-content/50" title="Pages served by the ampr debug API on port 9090">API :9090</span>
            <div class="join flex flex-wrap">
              {#each apiSources as source (source.id)}
                <button
                  class="btn btn-xs join-item"
                  class:btn-primary={selected === source.id}
                  class:btn-ghost={selected !== source.id}
                  aria-pressed={selected === source.id}
                  disabled={!amprRunning}
                  title={!amprRunning ? 'Start the game with the debug build to use the 9090 API' : undefined}
                  onclick={() => loadLog(source.id)}
                >
                  {source.label}
                </button>
              {/each}
            </div>
          </div>
          <div class="flex flex-wrap items-center gap-2">
            <span class="w-24 shrink-0 text-[11px] font-semibold uppercase tracking-wide text-base-content/50" title="Files read from /data: ampr_emu_trace.txt and ampr_emu_tune.txt">Local files</span>
            <div class="join flex flex-wrap">
              {#each fileSources as source (source.id)}
                <button
                  class="btn btn-xs join-item"
                  class:btn-primary={selected === source.id}
                  class:btn-ghost={selected !== source.id}
                  aria-pressed={selected === source.id}
                  onclick={() => loadLog(source.id)}
                >
                  {source.label}
                </button>
              {/each}
            </div>
          </div>
        </div>
        <div class="flex items-center gap-2 pt-1">
          <span class="text-xs text-base-content/50">{logText ? formatBytes(logSize) : ''}</span>
          <button class="btn btn-xs" onclick={() => loadLog(selected)} disabled={logLoading}>
            {#if logLoading}<span class="loading loading-spinner loading-xs"></span>{/if}
            Reload
          </button>
        </div>
      </div>

      <pre
        bind:this={logPane}
        class="h-[65vh] min-h-80 overflow-auto rounded-box bg-neutral p-4 font-mono text-xs leading-relaxed text-neutral-content whitespace-pre-wrap break-words">{logText ||
          'Choose a source above. The 9090 pages need the debug build and a running game; the trace works after a crash on start.'}</pre>
    </div>
  </div>
</div>
