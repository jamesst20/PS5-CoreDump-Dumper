<!-- SPDX-License-Identifier: MIT -->
<script>
  import { onMount } from 'svelte';
  import DebugTab from './DebugTab.svelte';

  let dumps = $state([]);
  let tab = $state('dumps');
  let version = $state('');
  let systemLog = $state('');
  let systemLogLoading = $state(false);
  let busy = $state(false);
  let error = $state('');
  let expanded = $state('');
  let systemLogPane = $state();
  let debugTab = $state();

  let totalBytes = $derived(dumps.reduce((sum, dump) => sum + dump.bytes, 0));
  let sectionTitle = $derived(
    tab === 'dumps' ? 'Coredumps' : tab === 'logs' ? 'System logs' : 'Ampr-emu-ng - Debug',
  );

  // dumper.log is appended to, so open at the latest line.
  $effect(() => {
    tab;
    systemLog;
    if (systemLogPane) {
      systemLogPane.scrollTop = systemLogPane.scrollHeight;
    }
  });

  async function request(path, options) {
    const response = await fetch(path, options);
    if (!response.ok) {
      throw new Error(`${response.status} ${response.statusText}: ${(await response.text()).slice(0, 200)}`);
    }
    return response.json();
  }

  async function loadVersion() {
    try {
      version = (await request('/api/version')).version ?? '';
    } catch {
      version = '';
    }
  }

  async function loadDumps() {
    try {
      dumps = (await request('/api/dumps')).dumps;
      error = '';
    } catch (cause) {
      error = String(cause.message ?? cause);
    }
  }

  async function loadSystemLog() {
    systemLogLoading = true;
    try {
      const response = await fetch('/api/log?name=dumper.log');
      if (response.status === 404) {
        systemLog = 'No system log yet.';
      } else if (!response.ok) {
        throw new Error(`${response.status} ${response.statusText}: ${(await response.text()).slice(0, 200)}`);
      } else {
        systemLog = await response.text();
      }
      error = '';
    } catch (cause) {
      systemLog = '';
      error = String(cause.message ?? cause);
    }
    systemLogLoading = false;
  }

  async function selectSection(section) {
    tab = section;
    if (section === 'dumps') {
      await loadDumps();
    } else if (section === 'logs') {
      await loadSystemLog();
    }
  }

  async function refreshActive() {
    busy = true;
    if (tab === 'debug') {
      await debugTab?.refresh();
    } else if (tab === 'logs') {
      await loadSystemLog();
    } else {
      await loadDumps();
    }
    busy = false;
  }

  async function removeDump(name) {
    if (!confirm(`Delete ${name}?\n\nThis cannot be undone.`)) {
      return;
    }
    try {
      await request(`/api/dumps/${encodeURIComponent(name)}`, { method: 'DELETE' });
      expanded = '';
      await loadDumps();
    } catch (cause) {
      error = String(cause.message ?? cause);
    }
  }

  function onShowDumps() {
    selectSection('dumps');
  }

  function navClass(section) {
    return `flex w-full items-center gap-2.5 rounded-lg px-3 py-2 text-sm transition-colors ${
      tab === section ? 'bg-primary text-primary-content' : 'text-base-content/80 hover:bg-base-200'
    }`;
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
    loadVersion();
    loadDumps();
  });
</script>

<div class="flex min-h-screen flex-col bg-base-200 lg:flex-row">
  <aside class="flex w-full shrink-0 flex-col border-b border-base-300 bg-base-100 lg:sticky lg:top-0 lg:h-screen lg:w-64 lg:border-b-0 lg:border-r">
    <div class="flex items-center gap-3 px-4 py-5">
      <div class="flex size-10 shrink-0 items-center justify-center rounded-box bg-primary text-primary-content">
        <svg class="size-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true">
          <path d="M4 7c0-1.7 3.6-3 8-3s8 1.3 8 3-3.6 3-8 3-8-1.3-8-3Z" />
          <path d="M4 7v10c0 1.7 3.6 3 8 3s8-1.3 8-3V7" />
          <path d="M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3" />
        </svg>
      </div>
      <div class="min-w-0">
        <div class="flex items-baseline gap-1.5">
          <h1 class="truncate text-sm font-semibold leading-tight">CoreDump Dumper</h1>
          {#if version}<span class="text-[11px] text-base-content/50">v{version}</span>{/if}
        </div>
        <p class="text-xs text-base-content/60">PlayStation 5 crash dumps</p>
      </div>
    </div>

    <nav class="flex flex-col gap-1 px-3">
      <button class={navClass('dumps')} onclick={() => selectSection('dumps')}>
        <svg class="size-4 shrink-0" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true">
          <path d="M4 7c0-1.7 3.6-3 8-3s8 1.3 8 3-3.6 3-8 3-8-1.3-8-3Z" />
          <path d="M4 7v10c0 1.7 3.6 3 8 3s8-1.3 8-3V7" />
          <path d="M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3" />
        </svg>
        <span>Coredumps</span>
        <span class="ml-auto rounded-full px-1.5 py-0.5 text-[10px] {tab === 'dumps' ? 'bg-primary-content/20' : 'bg-base-200'}">{dumps.length}</span>
      </button>
      <button class={navClass('debug')} onclick={() => selectSection('debug')}>
        <svg class="size-4 shrink-0" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true">
          <path d="M3 12h4l2-7 4 14 2-7h6" />
        </svg>
        <span>Ampr-emu-ng - Debug</span>
      </button>
    </nav>

    <div class="mt-auto border-t border-base-300 p-3">
      <button class={navClass('logs')} onclick={() => selectSection('logs')}>
        <svg class="size-4 shrink-0" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true">
          <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8Z" />
          <path d="M14 2v6h6" />
          <path d="M8 13h8" />
          <path d="M8 17h8" />
        </svg>
        <span>System logs</span>
      </button>
    </div>
  </aside>

  <main class="min-w-0 flex-1">
    <div class="mx-auto flex max-w-6xl flex-col gap-4 px-4 py-6 lg:px-8">
      <div class="flex items-center justify-between gap-3">
        <h2 class="text-lg font-semibold">{sectionTitle}</h2>
        <button class="btn btn-sm btn-ghost" onclick={refreshActive} disabled={busy}>
          {#if busy}
            <span class="loading loading-spinner loading-xs"></span>
          {:else}
            <svg class="size-4" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true">
              <path d="M20 11a8 8 0 1 0-2.3 5.7" />
              <path d="M20 4v7h-7" />
            </svg>
          {/if}
          Refresh
        </button>
      </div>

      {#if error}
        <div class="alert alert-error text-sm">
          <span class="min-w-0 break-words">{error}</span>
          <button class="btn btn-ghost btn-xs" onclick={() => (error = '')}>Dismiss</button>
        </div>
      {/if}

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
      {:else if tab === 'logs'}
        <div class="card border border-base-300 bg-base-100 shadow-sm">
          <div class="card-body gap-3">
            <div class="flex flex-wrap items-center gap-2">
              <h3 class="text-sm font-semibold">System log</h3>
              <span class="font-mono text-xs text-base-content/50">dumper.log</span>
              <button class="btn btn-xs ml-auto" onclick={loadSystemLog} disabled={systemLogLoading}>
                {#if systemLogLoading}<span class="loading loading-spinner loading-xs"></span>{/if}
                Reload
              </button>
            </div>
            <pre
              bind:this={systemLogPane}
              class="h-[70vh] min-h-80 overflow-auto rounded-box bg-neutral p-4 font-mono text-xs leading-relaxed text-neutral-content whitespace-pre-wrap break-words">{systemLog ||
                (systemLogLoading ? 'Loading…' : 'No system log yet.')}</pre>
          </div>
        </div>
      {:else}
        <DebugTab bind:this={debugTab} onShowDumps={onShowDumps} />
      {/if}
    </div>
  </main>
</div>
