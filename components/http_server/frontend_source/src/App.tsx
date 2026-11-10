import { createEffect, createSignal, lazy, onCleanup, onMount, Show, Suspense } from 'solid-js';
import type { Component } from 'solid-js';

import { fetchStatus, restartDevice } from './api/client';
import { Layout } from './components/layout/Layout';
import { LEAF_IDS } from './components/layout/Sidebar';
import { RestartOverlay, type RestartOverlayState } from './components/system/RestartOverlay';
import { Banner } from './components/ui/Banner';
import { ToastViewport } from './components/ui/ToastViewport';
import { t } from './i18n';
import { anyDirty, type TabId } from './state/dirty';
import {
  appCapabilities,
  appStatus,
  capabilitiesLoaded,
  reloadCapabilities,
  reloadLuaModules,
  reloadStatus,
} from './state/config';
import { restartAvailable, routeAvailable, routeNeedsCapabilities } from './state/features';
import { pushToast } from './state/toast';

const StatusPage = lazy(() =>
  import('./pages/StatusPage').then((mod) => ({ default: mod.StatusPage })),
);
const BasicPage = lazy(() =>
  import('./pages/BasicPage').then((mod) => ({ default: mod.BasicPage })),
);
const WebReqPage = lazy(() =>
  import('./pages/WebReqPage').then((mod) => ({ default: mod.WebReqPage })),
);
const MemoryPage = lazy(() =>
  import('./pages/MemoryPage').then((mod) => ({ default: mod.MemoryPage })),
);
const LlmPage = lazy(() => import('./pages/LlmPage').then((mod) => ({ default: mod.LlmPage })));
const AsrPage = lazy(() => import('./pages/AsrPage').then((mod) => ({ default: mod.AsrPage })));
const ImPage = lazy(() => import('./pages/ImPage').then((mod) => ({ default: mod.ImPage })));
const CapabilitiesPage = lazy(() =>
  import('./pages/CapabilitiesPage').then((mod) => ({ default: mod.CapabilitiesPage })),
);
const SkillsPage = lazy(() =>
  import('./pages/SkillsPage').then((mod) => ({ default: mod.SkillsPage })),
);
const FilesPage = lazy(() =>
  import('./pages/FilesPage').then((mod) => ({ default: mod.FilesPage })),
);
const WebImPage = lazy(() =>
  import('./pages/WebImPage').then((mod) => ({ default: mod.WebImPage })),
);
const MatterPage = lazy(() =>
  import('./pages/MatterPage').then((mod) => ({ default: mod.MatterPage })),
);
const SetupWizardPage = lazy(() =>
  import('./pages/SetupWizardPage').then((mod) => ({ default: mod.SetupWizardPage })),
);

const MatterBridgePage = lazy(() =>
  import('./pages/MatterBridgePage').then((mod) => ({ default: mod.MatterBridgePage })),
);

type RouteId = TabId | 'start';
type RestartRequestOptions = {
  targetTab?: TabId;
  reloadOnSuccess?: boolean;
};

function readTabFromHash(): RouteId {
  const hash = window.location.hash.replace(/^#\/?/, '');
  if (hash === 'search') return 'webreq';
  if (hash === 'start') return 'start';
  return LEAF_IDS.includes(hash as TabId) ? (hash as TabId) : 'status';
}

const App: Component = () => {
  const [currentTab, setCurrentTab] = createSignal<RouteId>(readTabFromHash());
  const [bootError, setBootError] = createSignal<string | null>(null);
  const [restartOverlay, setRestartOverlay] = createSignal<RestartOverlayState>({
    open: false,
    phase: 'requesting',
    error: null,
  });
  let restartTimer: ReturnType<typeof setTimeout> | null = null;
  let pollController: AbortController | null = null;
  let restartStartedAt = 0;
  let restartTarget: TabId | null = null;
  let restartReloadOnSuccess = false;
  let restartActive = false;

  const clearRestartFlow = () => {
    restartActive = false;
    if (restartTimer) {
      clearTimeout(restartTimer);
      restartTimer = null;
    }
    pollController?.abort();
    pollController = null;
    restartReloadOnSuccess = false;
  };

  const closeRestartOverlay = () => {
    clearRestartFlow();
    setRestartOverlay({ open: false, phase: 'requesting', error: null });
  };

  const onHashChange = () => {
    const next = readTabFromHash();
    if (next === currentTab()) return;
    if (anyDirty()) {
      const ok = window.confirm(t('unsavedConfirmLeave') as string);
      if (!ok) {
        window.location.hash = '#' + currentTab();
        return;
      }
    }
    setCurrentTab(next);
  };

  onMount(() => {
    window.addEventListener('hashchange', onHashChange);
    void bootstrap();
  });

  onCleanup(() => {
    clearRestartFlow();
    window.removeEventListener('hashchange', onHashChange);
  });

  createEffect(() => {
    window.location.hash = '#' + currentTab();
  });

  createEffect(() => {
    if (
      appStatus() &&
      (!appStatus()?.claw_enabled || !routeNeedsCapabilities(currentTab()) || capabilitiesLoaded()) &&
      !routeAvailable(currentTab(), appStatus(), appCapabilities())
    ) {
      setCurrentTab(currentTab() === 'start' ? 'basic' : 'status');
    }
  });

  const handleSelectTab = (next: TabId) => {
    setCurrentTab(next);
  };

  const bootstrap = async () => {
    try {
      await reloadStatus();
    } catch (err) {
      setBootError((err as Error).message);
      return;
    }
    if (!appStatus()?.claw_enabled) return;
    const tasks: Array<[string, () => Promise<unknown>]> = [
      ['capabilities', () => reloadCapabilities()],
      ['luaModules', () => reloadLuaModules()],
    ];
    for (const [label, task] of tasks) {
      try {
        await task();
      } catch (err) {
        const message = (err as Error).message || 'Failed to initialise: ' + label;
        if (label === 'capabilities') setBootError(message);
        pushToast(message, 'error', 4500);
      }
    }
  };

  const scheduleNextPoll = () => {
    if (!restartActive) return;
    const deadlineAt = restartStartedAt + 30000;
    if (Date.now() >= deadlineAt) {
      setRestartOverlay({
        open: true,
        phase: 'error',
        error: t('restartOverlayTimeout') as string,
        deadlineAt,
      });
      return;
    }

    restartTimer = setTimeout(async () => {
      if (!restartActive) return;
      pollController?.abort();
      pollController = new AbortController();
      try {
        await fetchStatus(pollController.signal);
        if (restartReloadOnSuccess) {
          window.location.reload();
          return;
        }
        await reloadStatus();
        closeRestartOverlay();
        if (restartTarget) {
          setCurrentTab(restartTarget);
        }
      } catch (err) {
        if (!restartActive) return;
        if ((err as Error).name !== 'AbortError') {
          setRestartOverlay({
            open: true,
            phase: 'polling',
            error: null,
            deadlineAt,
          });
        }
        scheduleNextPoll();
      }
    }, 2000);
  };

  const handleRestartRequest = async (options?: RestartRequestOptions) => {
    if (!restartAvailable(appStatus())) return;
    clearRestartFlow();
    restartActive = true;
    restartTarget = options?.targetTab ?? null;
    restartReloadOnSuccess = options?.reloadOnSuccess ?? false;
    restartStartedAt = Date.now();
    const deadlineAt = restartStartedAt + 30000;
    setRestartOverlay({
      open: true,
      phase: 'requesting',
      error: null,
      deadlineAt,
    });

    try {
      await restartDevice();
      setRestartOverlay({
        open: true,
        phase: 'cooldown',
        error: null,
        deadlineAt,
      });
      restartTimer = setTimeout(() => {
        setRestartOverlay({
          open: true,
          phase: 'polling',
          error: null,
          deadlineAt,
        });
        scheduleNextPoll();
      }, 5000);
    } catch (err) {
      setRestartOverlay({
        open: true,
        phase: 'error',
        error: (err as Error).message,
        deadlineAt,
      });
    }
  };

  return (
    <>
      <Show
        when={currentTab() === 'start' && appStatus()?.claw_enabled === true}
        fallback={
          <Layout currentTab={currentTab() as TabId} onSelectTab={handleSelectTab}>
            <Show when={bootError()}>
              <div class="mb-4">
                <Banner kind="error" message={bootError() ?? undefined} />
              </div>
            </Show>
            <Suspense
              fallback={<div class="p-6 text-[var(--color-text-muted)]">{t('statusLoading')}</div>}
            >
              <Show when={appStatus() && routeAvailable(currentTab(), appStatus(), appCapabilities())}>
              <Show when={currentTab() === 'status'}>
                <StatusPage onRestartRequest={() => void handleRestartRequest()} />
              </Show>
              <Show when={currentTab() === 'basic'}>
                <BasicPage
                  onRestartRequest={() => void handleRestartRequest({ reloadOnSuccess: true })}
                />
              </Show>
              <Show when={currentTab() === 'llm'}>
                <LlmPage />
              </Show>
              <Show when={currentTab() === 'asr'}>
                <AsrPage />
              </Show>
              <Show when={currentTab() === 'im'}>
                <ImPage />
              </Show>
              <Show when={currentTab() === 'webreq'}>
                <WebReqPage
                  onRestartRequest={() => void handleRestartRequest({ reloadOnSuccess: true })}
                />
              </Show>
              <Show when={currentTab() === 'memory'}>
                <MemoryPage />
              </Show>
              <Show when={currentTab() === 'webim'}>
                <WebImPage />
              </Show>
              <Show when={currentTab() === 'matter'}>
                <Show when={appStatus()?.matter_role === 'bridge'} fallback={<MatterPage />}>
                  <MatterBridgePage />
                </Show>
              </Show>
              <Show when={currentTab() === 'capabilities'}>
                <CapabilitiesPage
                  onRestartRequest={() => void handleRestartRequest({ reloadOnSuccess: true })}
                />
              </Show>
              <Show when={currentTab() === 'skills'}>
                <SkillsPage
                  onRestartRequest={() => void handleRestartRequest({ reloadOnSuccess: true })}
                />
              </Show>
              <Show when={currentTab() === 'files'}>
                <FilesPage />
              </Show>
              </Show>
            </Suspense>
          </Layout>
        }
      >
        <Suspense
          fallback={<div class="p-6 text-[var(--color-text-muted)]">{t('statusLoading')}</div>}
        >
          <Show
            when={capabilitiesLoaded()}
            fallback={<Banner kind={bootError() ? 'error' : 'info'} message={bootError() ?? (t('statusLoading') as string)} />}
          >
            <SetupWizardPage
              onRestartRequest={(targetTab) => void handleRestartRequest({ targetTab })}
            />
          </Show>
        </Suspense>
      </Show>
      <RestartOverlay state={restartOverlay()} onClose={closeRestartOverlay} />
      <ToastViewport />
    </>
  );
};

export default App;
