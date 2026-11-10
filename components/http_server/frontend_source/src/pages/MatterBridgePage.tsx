import { createEffect, createSignal, For, onCleanup, onMount, Show } from 'solid-js';
import { generate } from 'lean-qr';
import {
  fetchMatterBridge, fetchMatterBridgeDevices, openMatterBridgeWindow,
  type MatterBridgeDevice, type MatterBridgeInfo,
} from '../api/client';
import { TabShell } from '../components/layout/TabShell';
import { Banner } from '../components/ui/Banner';
import { Button } from '../components/ui/Button';
import { PageHeader } from '../components/ui/PageHeader';
import { Section } from '../components/ui/Section';
import { t } from '../i18n';
import { pushToast } from '../state/toast';

const PairingQr = (props: { payload: string }) => {
  let canvas!: HTMLCanvasElement;
  createEffect(() => {
    generate(props.payload).toCanvas(canvas);
  });
  return <canvas ref={canvas} aria-label={t('bridgePairing') as string}
    class="block w-52 h-52 rounded-xl bg-white p-3"
    style={{ 'image-rendering': 'pixelated' }} />;
};

export const MatterBridgePage = () => {
  const [info, setInfo] = createSignal<MatterBridgeInfo>();
  const [devices, setDevices] = createSignal<MatterBridgeDevice[]>([]);
  const [error, setError] = createSignal<string | null>(null);
  const [refreshing, setRefreshing] = createSignal(false);
  const [opening, setOpening] = createSignal(false);
  let timer: ReturnType<typeof setTimeout> | undefined;
  let controller: AbortController | undefined;
  let active = true;

  const refresh = async () => {
    if (refreshing() || !active) return;
    setRefreshing(true);
    controller = new AbortController();
    try {
      const [nextInfo, nextDevices] = await Promise.all([
        fetchMatterBridge(controller.signal), fetchMatterBridgeDevices(controller.signal),
      ]);
      if (!active) return;
      setInfo(nextInfo);
      setDevices(nextDevices);
      setError(null);
    } catch (err) {
      if (active && (err as Error).name !== 'AbortError') setError((err as Error).message);
    } finally {
      if (active) setRefreshing(false);
    }
  };

  const poll = async () => {
    if (!document.hidden) await refresh();
    if (active) timer = setTimeout(() => void poll(), 3000);
  };
  onMount(() => void poll());
  onCleanup(() => {
    active = false;
    clearTimeout(timer);
    controller?.abort();
  });

  const openWindow = async () => {
    setOpening(true);
    try {
      await openMatterBridgeWindow();
      if (active) {
        pushToast(t('bridgeWindowOpened') as string, 'success');
        await refresh();
      }
    } catch (err) {
      if (active) setError((err as Error).message);
    } finally {
      if (active) setOpening(false);
    }
  };

  return (
    <TabShell>
      <PageHeader title={t('bridgeTitle') as string} description={t('bridgeDescription') as string}
        actions={<Button size="sm" variant="secondary" disabled={refreshing()}
          onClick={() => void refresh()}>{t('sysInfoReload')}</Button>} />
      <Show when={error()}>
        <div class="px-5 py-3"><Banner kind="error" message={error()!} /></div>
      </Show>
      <Show when={info()} fallback={<p class="p-5 text-sm">{t('statusLoading')}</p>}>
        {(bridge) => <>
          <Section title={t('bridgeTitle')}>
            <p class="text-sm">{bridge().commissioned ? t('bridgeCommissioned') : t('bridgeUncommissioned')}</p>
          </Section>
          <Show when={!bridge().commissioned}>
            <Section title={t('bridgePairing')} note={t('bridgePairingHint')}>
              <div class="flex flex-col items-start gap-4 sm:flex-row sm:items-center">
                <Show when={bridge().qr_code}>
                  <PairingQr payload={bridge().qr_code} />
                </Show>
                <div class="space-y-4">
                  <p class="font-mono text-2xl tracking-widest">{bridge().manual_pairing_code}</p>
                  <Button disabled={opening()} onClick={() => void openWindow()}>
                    {t('bridgeOpenWindow')}
                  </Button>
                </div>
              </div>
            </Section>
          </Show>
          <Section title={t('bridgeDevices')}>
            <div class="space-y-2">
              <For each={devices()} fallback={<p class="text-sm text-[var(--color-text-muted)]">{t('bridgeEmpty')}</p>}>
                {(device) => (
                  <div class="flex flex-wrap justify-between gap-3 rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] p-3">
                    <div>
                      <p class="font-semibold">{device.name || device.id}</p>
                      <p class="text-xs text-[var(--color-text-muted)]">{device.profile_id}</p>
                    </div>
                    <div class="text-right text-sm">
                      <p>{t('bridgeEndpoint')}: <span class="font-mono">{device.endpoint_id}</span></p>
                      <p>{!device.active ? t('bridgeInactive') : device.reachable ? t('matterOnline') : t('matterOffline')}</p>
                    </div>
                  </div>
                )}
              </For>
            </div>
          </Section>
        </>}
      </Show>
    </TabShell>
  );
};
