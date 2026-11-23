<!-- SPDX-License-Identifier: MIT -->
<script>
  import { onMount } from 'svelte';

  let dumps = $state([]);
  let logs = $state([]);
  let tab = $state('dumps');
  let selectedLog = $state('dumper.log');
  let logText = $state('');
  let busy = $state(false);
  let error = $state('');
  let expanded = $state('');
  let logPane;

  // New log content is appended at the bottom, so open at the latest line.
  $effect(() => {
    tab;
    logText;
    if (logPane) {
      logPane.scrollTop = logPane.scrollHeight;
    }
  });

  let totalBytes = $derived(dumps.reduce((sum, dump) => sum + dump.bytes, 0));

  async function request(path, options) {
    const response = await fetch(path, options);
    if (!response.ok) {
      throw new Error(`${response.status} ${response.statusText}: ${(await response.text()).slice(0, 200)}`);
    }
    return response.json();
  }

  async function refresh() {
    busy = true;
    try {
      const [dumpReply, logReply] = await Promise.all([request('/api/dumps'), request('/api/logs')]);
      dumps = dumpReply.dumps;
      logs = logReply.logs;
      if (!logs.some((log) => log.name === selectedLog)) {
        selectedLog = logs[0]?.name ?? '';
      }
      error = '';
    } catch (cause) {
      error = String(cause.message ?? cause);
    }
    busy = false;
  }

  async function openLog(name) {
    selectedLog = name;
    logText = 'Loading…';
    try {
      const response = await fetch(`/api/log?name=${encodeURIComponent(name)}`);
      if (!response.ok) {
        throw new Error(`${response.status} ${response.statusText}`);
      }
      logText = await response.text();
      error = '';
    } catch (cause) {
      logText = '';
      error = String(cause.message ?? cause);
    }
  }

  async function removeDump(name) {
    if (!confirm(`Delete ${name}?\n\nThis cannot be undone.`)) {
      return;
    }
    try {
      await request(`/api/dumps/${encodeURIComponent(name)}`, { method: 'DELETE' });
      expanded = '';
      await refresh();
    } catch (cause) {
      error = String(cause.message ?? cause);
    }
  }

  function showLogs() {
    tab = 'logs';
    if (selectedLog && !logText) {
      openLog(selectedLog);
    }
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
    return new Date(seconds * 1000).toLocaleString();
  }

  onMount(() => {
    refresh();
  });
</script>

<div class="min-h-screen bg-base-200">
  <header class="border-b border-base-300 bg-base-100">
    <div class="mx-auto flex max-w-5xl items-center gap-3 px-4 py-4">
      <div class="flex size-10 items-center justify-center rounded-box bg-primary text-primary-content">
        <svg class="size-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8">
          <path d="M4 7c0-1.7 3.6-3 8-3s8 1.3 8 3-3.6 3-8 3-8-1.3-8-3Z" />
          <path d="M4 7v10c0 1.7 3.6 3 8 3s8-1.3 8-3V7" />
          <path d="M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3" />
        </svg>
      </div>
      <div class="mr-auto">
        <h1 class="text-lg font-semibold leading-tight">CoreDump Dumper</h1>
        <p class="text-xs text-base-content/60">PlayStation 5 crash dumps</p>
      </div>
      <button class="btn btn-sm btn-ghost" onclick={refresh} disabled={busy}>
        {#if busy}
          <span class="loading loading-spinner loading-xs"></span>
        {:else}
          <svg class="size-4" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M20 11a8 8 0 1 0-2.3 5.7" />
            <path d="M20 4v7h-7" />
          </svg>
        {/if}
        Refresh
      </button>
    </div>
  </header>

  <main class="mx-auto flex max-w-5xl flex-col gap-4 px-4 py-6">
    {#if error}
      <div class="alert alert-error text-sm">
        <span class="min-w-0 break-words">{error}</span>
        <button class="btn btn-ghost btn-xs" onclick={() => (error = '')}>Dismiss</button>
      </div>
    {/if}

    <div role="tablist" class="tabs tabs-box w-fit">
      <button role="tab" class="tab gap-2" class:tab-active={tab === 'dumps'} onclick={() => (tab = 'dumps')}>
        Coredumps
        <span class="badge badge-sm">{dumps.length}</span>
      </button>
      <button role="tab" class="tab" class:tab-active={tab === 'logs'} onclick={showLogs}>Logs</button>
    </div>

    {#if tab === 'dumps'}
      <div class="stats stats-horizontal w-full border border-base-300 bg-base-100 shadow-sm">
        <div class="stat">
          <div class="stat-title">Coredumps</div>
          <div class="stat-value text-2xl">{dumps.length}</div>
        </div>
        <div class="stat">
          <div class="stat-title">Stored</div>
          <div class="stat-value text-2xl">{formatBytes(totalBytes)}</div>
        </div>
      </div>

      <div class="card border border-base-300 bg-base-100 shadow-sm">
        <div class="card-body p-0">
          {#if dumps.length === 0}
            <div class="px-6 py-12 text-center">
              <p class="text-sm font-medium">No coredumps saved yet</p>
              <p class="mt-1 text-xs text-base-content/60">
                A dump appears here as soon as a title crashes on the console.
              </p>
            </div>
          {:else}
            <div>
              <table class="table">
                <thead>
                  <tr>
                    <th>Coredump</th>
                    <th>Size</th>
                    <th>Files</th>
                    <th>Saved</th>
                    <th class="text-right">Actions</th>
                  </tr>
                </thead>
                <tbody>
                  {#each dumps as dump (dump.name)}
                    <tr>
                      <td class="max-w-xs">
                        <button
                          class="link link-hover truncate font-mono text-sm"
                          onclick={() => (expanded = expanded === dump.name ? '' : dump.name)}
                        >
                          {dump.name}
                        </button>
                      </td>
                      <td class="whitespace-nowrap">{formatBytes(dump.bytes)}</td>
                      <td>{dump.files}</td>
                      <td class="whitespace-nowrap text-sm">{formatTime(dump.mtime)}</td>
                      <td class="text-right whitespace-nowrap">
                        {#if dump.video}
                          <details class="dropdown dropdown-end inline-block">
                            <summary class="btn btn-primary btn-xs list-none">
                              Download
                              <svg class="size-3" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                                <path d="m6 9 6 6 6-6" />
                              </svg>
                            </summary>
                            <ul class="dropdown-content menu z-20 mt-1 w-56 rounded-box bg-base-100 p-2 shadow">
                              <li>
                                <a
                                  href={`/api/dumps/${encodeURIComponent(dump.name)}`}
                                  download={`${dump.name}.tar`}
                                >
                                  Download
                                </a>
                              </li>
                              <li>
                                <a
                                  href={`/api/dumps/${encodeURIComponent(dump.name)}?novideo=1`}
                                  download={`${dump.name}-no-video.tar`}
                                >
                                  Download without video
                                </a>
                              </li>
                            </ul>
                          </details>
                        {:else}
                          <a
                            class="btn btn-primary btn-xs"
                            href={`/api/dumps/${encodeURIComponent(dump.name)}`}
                            download={`${dump.name}.tar`}
                          >
                            Download
                          </a>
                        {/if}
                        <button class="btn btn-error btn-outline btn-xs" onclick={() => removeDump(dump.name)}>
                          Delete
                        </button>
                      </td>
                    </tr>
                    {#if expanded === dump.name}
                      <tr>
                        <td colspan="5" class="bg-base-200/50">
                          <div class="flex flex-col gap-1 py-1">
                            {#each dump.items as file (file.name)}
                              <a
                                class="link link-hover flex items-baseline justify-between gap-4 font-mono text-xs"
                                href={`/api/dumps/${encodeURIComponent(dump.name)}/${encodeURIComponent(file.name)}`}
                                download={file.name}
                              >
                                <span class="truncate">{file.name}</span>
                                <span class="shrink-0 text-base-content/50">{formatBytes(file.bytes)}</span>
                              </a>
                            {:else}
                              <span class="text-xs text-base-content/60">No files.</span>
                            {/each}
                          </div>
                        </td>
                      </tr>
                    {/if}
                  {/each}
                </tbody>
              </table>
            </div>
          {/if}
        </div>
      </div>
    {:else}
      <div class="card border border-base-300 bg-base-100 shadow-sm">
        <div class="card-body gap-3">
          <div class="flex flex-wrap items-center gap-2">
            <select
              class="select select-sm w-full max-w-md"
              value={selectedLog}
              onchange={(event) => openLog(event.currentTarget.value)}
              disabled={logs.length === 0}
            >
              {#each logs as log (log.name)}
                <option value={log.name}>{log.name}</option>
              {/each}
            </select>
            <button class="btn btn-sm" onclick={() => openLog(selectedLog)} disabled={!selectedLog}>Reload</button>
          </div>

          {#if logs.length === 0}
            <p class="text-sm text-base-content/60">No logs yet.</p>
          {/if}

          <pre
            bind:this={logPane}
            class="max-h-[60vh] overflow-auto rounded-box bg-neutral p-4 text-xs leading-relaxed text-neutral-content whitespace-pre-wrap break-words">{logText || 'Select a log to view it.'}</pre>
        </div>
      </div>
    {/if}
  </main>
</div>
