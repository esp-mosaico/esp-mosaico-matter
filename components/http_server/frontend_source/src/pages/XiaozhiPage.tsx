import { ExternalLink, KeyRound, RefreshCw } from 'lucide-solid';
import {
  createEffect,
  createSignal,
  onCleanup,
  onMount,
  Show,
  type Component,
  type JSX,
} from 'solid-js';
import { fetchXiaozhiStatus, type XiaozhiStatusInfo } from '../api/client';
import { TabShell } from '../components/layout/TabShell';
import { Banner } from '../components/ui/Banner';
import { Button } from '../components/ui/Button';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { PageHeader } from '../components/ui/PageHeader';
import { XIAOZHI_CONSOLE_URL } from '../constants/externalLinks';
import { t } from '../i18n';
import { pushToast } from '../state/toast';

const InfoRow: Component<{ label: string; value?: JSX.Element | string; mono?: boolean }> = (
  props,
) => {
  const none = t('sysInfoNone') as string;
  return (
    <div class="flex items-center justify-between py-2 px-3 rounded-[var(--radius-sm)] bg-white/[0.02] border border-transparent hover:border-[var(--color-border-subtle)] gap-3">
      <span class="text-[0.78rem] uppercase tracking-wider text-[var(--color-text-muted)] font-semibold">
        {props.label}
      </span>
      <span
        class={[
          'text-[0.88rem] text-[var(--color-text-primary)] text-right break-all',
          props.mono ? 'font-mono' : '',
        ].join(' ')}
      >
        {props.value || none}
      </span>
    </div>
  );
};

function formatStateLabel(state?: string): string {
  switch (state) {
    case 'starting':
      return t('xiaozhiStateStarting') as string;
    case 'no_wifi':
      return t('xiaozhiStateNoWifi') as string;
    case 'activating':
      return t('xiaozhiStateActivating') as string;
    case 'connecting':
      return t('xiaozhiStateConnecting') as string;
    case 'connected':
      return t('xiaozhiStateConnected') as string;
    case 'disconnected':
      return t('xiaozhiStateDisconnected') as string;
    case 'error':
      return t('xiaozhiStateError') as string;
    default:
      return state || (t('sysInfoNone') as string);
  }
}

function stateHint(state?: string): string {
  switch (state) {
    case 'starting':
      return t('xiaozhiHintStarting') as string;
    case 'no_wifi':
      return t('xiaozhiHintNoWifi') as string;
    case 'activating':
      return t('xiaozhiHintActivating') as string;
    case 'connecting':
      return t('xiaozhiHintConnecting') as string;
    case 'connected':
      return t('xiaozhiHintConnected') as string;
    case 'disconnected':
      return t('xiaozhiHintDisconnected') as string;
    case 'error':
      return t('xiaozhiHintError') as string;
    default:
      return '';
  }
}

function stateDotClass(state?: string): string {
  switch (state) {
    case 'connected':
      return 'bg-emerald-400';
    case 'activating':
    case 'connecting':
    case 'starting':
    case 'no_wifi':
      return 'bg-amber-400';
    case 'disconnected':
    case 'error':
      return 'bg-rose-400';
    default:
      return 'bg-[var(--color-text-muted)]';
  }
}

function isPendingState(state?: string): boolean {
  return state === 'starting' || state === 'connecting' || state === 'activating';
}

function formatTransport(transport?: string): string | undefined {
  if (!transport) return undefined;
  if (transport === 'mqtt') return 'MQTT';
  if (transport === 'websocket') return 'WebSocket';
  return transport;
}

export const XiaozhiPage: Component = () => {
  const [info, setInfo] = createSignal<XiaozhiStatusInfo | null>(null);
  const [error, setError] = createSignal<string | null>(null);
  let pollTimer: ReturnType<typeof setInterval> | undefined;
  let controller: AbortController | undefined;
  let inFlight = false;
  let active = true;

  const loadStatus = async (): Promise<boolean> => {
    if (inFlight || !active) return false;
    inFlight = true;
    controller = new AbortController();
    try {
      const next = await fetchXiaozhiStatus(controller.signal);
      if (!active) return false;
      setInfo(next);
      setError(null);
      return true;
    } catch (err) {
      if (active && (err as Error).name !== 'AbortError') {
        setError((err as Error).message);
      }
      return false;
    } finally {
      inFlight = false;
    }
  };

  const reload = async () => {
    try {
      const next = await fetchXiaozhiStatus();
      if (!active) return;
      setInfo(next);
      setError(null);
      pushToast(t('sysInfoReload') as string, 'success');
    } catch (err) {
      if (!active) return;
      pushToast((err as Error).message, 'error');
    }
  };

  const stopPolling = () => {
    if (pollTimer !== undefined) {
      clearInterval(pollTimer);
      pollTimer = undefined;
    }
  };

  onMount(() => {
    void loadStatus();
  });

  createEffect(() => {
    const pending = isPendingState(info()?.state);
    if (pending && pollTimer === undefined) {
      pollTimer = setInterval(() => {
        if (!document.hidden) void loadStatus();
      }, 3000);
    } else if (!pending && pollTimer !== undefined) {
      stopPolling();
    }
  });

  onCleanup(() => {
    active = false;
    stopPolling();
    controller?.abort();
  });

  return (
    <TabShell>
      <PageHeader
        title={t('xiaozhiTitle') as string}
        description={t('xiaozhiDescription') as string}
        actions={
          <Button size="sm" variant="secondary" onClick={() => void reload()}>
            <RefreshCw class="w-3.5 h-3.5" aria-hidden="true" />
            {t('sysInfoReload')}
          </Button>
        }
      />

      <Show when={error()}>
        <div class="px-5 py-3">
          <Banner kind="error" message={error()!} />
        </div>
      </Show>

      <Show
        when={info()}
        fallback={<p class="p-5 text-sm text-[var(--color-text-muted)]">{t('statusLoading')}</p>}
      >
        {(status) => (
          <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
            <Show when={status().activating}>
              <StaticConfigBlock title={t('xiaozhiActivationSection') as string}>
                <div class="pt-2 space-y-3">
                  <p class="text-[0.85rem] text-[var(--color-text-secondary)] m-0">
                    {t('xiaozhiActivationHint')}
                  </p>
                  <div class="p-4 rounded-[var(--radius-sm)] bg-white/[0.03] border border-[var(--color-border-subtle)]">
                    <div class="inline-flex items-center gap-2 text-xs text-[var(--color-text-muted)] font-semibold">
                      <KeyRound class="w-3.5 h-3.5" aria-hidden="true" />
                      {t('xiaozhiActivationCode')}
                    </div>
                    <div class="font-mono text-3xl font-bold tracking-[0.2em] text-[var(--color-text-primary)] mt-2 select-all break-all">
                      {status().activation_code || '------'}
                    </div>
                    <Show when={status().activation_message}>
                      <p class="text-xs text-[var(--color-text-muted)] mt-1 m-0">
                        {status().activation_message}
                      </p>
                    </Show>
                  </div>
                </div>
              </StaticConfigBlock>
            </Show>

            <StaticConfigBlock title={t('xiaozhiStatusSection') as string}>
              <div class="grid gap-2 sm:grid-cols-2 pt-2">
                <InfoRow
                  label={t('xiaozhiFieldServer') as string}
                  value={
                    <span class="inline-flex items-center gap-2">
                      <span
                        class={[
                          'w-2 h-2 rounded-full shrink-0',
                          stateDotClass(status().state),
                        ].join(' ')}
                      />
                      <span>{formatStateLabel(status().state)}</span>
                    </span>
                  }
                />
                <InfoRow
                  label={t('xiaozhiFieldTransport') as string}
                  value={formatTransport(status().transport)}
                  mono
                />
                <p
                  class="sm:col-span-2 m-0 px-1 pt-2 text-sm leading-relaxed text-[var(--color-text-secondary)]"
                  role="status"
                >
                  {stateHint(status().state)}
                </p>
                <Show when={status().last_error}>
                  <div class="sm:col-span-2">
                    <InfoRow
                      label={t('xiaozhiFieldLastError') as string}
                      value={status().last_error}
                      mono
                    />
                  </div>
                </Show>
              </div>
            </StaticConfigBlock>

            <StaticConfigBlock title={t('xiaozhiConsoleSection') as string}>
              <div class="pt-2 space-y-3">
                <p class="text-[0.85rem] text-[var(--color-text-secondary)] m-0">
                  {t('xiaozhiConsoleHint')}
                </p>
                <a
                  href={XIAOZHI_CONSOLE_URL}
                  target="_blank"
                  rel="noopener noreferrer"
                  class="inline-flex items-center justify-center gap-2 rounded-[var(--radius-sm)] bg-[var(--color-accent)] px-4 py-2 text-sm font-medium text-[var(--color-text-on-accent)] transition hover:opacity-90"
                >
                  {t('xiaozhiOpenConsole')}
                  <ExternalLink class="w-3.5 h-3.5 shrink-0" aria-hidden="true" />
                </a>
              </div>
            </StaticConfigBlock>
          </div>
        )}
      </Show>
    </TabShell>
  );
};
