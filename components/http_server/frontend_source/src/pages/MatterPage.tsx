import { createEffect, createMemo, createSignal, For, onCleanup, onMount, Show } from 'solid-js';
import {
  cancelMatterCommission,
  fetchMatterDevice,
  fetchMatterDevices,
  fetchMatterOperation,
  invokeMatterAction,
  pollMatterCommission,
  removeMatterDevice,
  startMatterCommission,
  subscribeMatterEvents,
  updateMatterDeviceIdentity,
  type MatterAction,
  type MatterDevice,
  type MatterDeviceDetail,
  type MatterEvent,
  type MatterProperty,
} from '../api/client';
import { TabShell } from '../components/layout/TabShell';
import { Button } from '../components/ui/Button';
import { SelectInput, TextInput } from '../components/ui/FormField';
import { PageHeader } from '../components/ui/PageHeader';
import { Section } from '../components/ui/Section';
import { pushToast } from '../state/toast';
import {
  brightnessArguments,
  colorArguments,
  colorTemperatureArguments,
  filterMatterDevices,
  findMatterAction,
  MATTER_ALL_ROOMS,
  matterOperationError,
  matterRoomFilters,
  normalizeMatterIdentity,
  powerArguments,
  reconcileMatterRoomFilter,
} from './matter-controls';

const reportError = (error: unknown) => pushToast((error as Error).message, 'error');
const findProperty = (device: MatterDeviceDetail, cluster: string, key: string) =>
  device.properties.find((property) => property.cluster === cluster && property.key === key);
const propertyKey = (property: Pick<MatterProperty, 'endpoint' | 'cluster' | 'key'>) =>
  `${property.endpoint}/${property.cluster}/${property.key}`;

export const MatterPage = () => {
  const [devices, setDevices] = createSignal<MatterDevice[]>([]);
  const [selected, setSelected] = createSignal<MatterDeviceDetail>();
  const [loading, setLoading] = createSignal(true);
  const [setupCode, setSetupCode] = createSignal('');
  const [commissioning, setCommissioning] = createSignal(false);
  const [name, setName] = createSignal('');
  const [room, setRoom] = createSignal('');
  const [powerOn, setPowerOn] = createSignal(false);
  const [brightness, setBrightness] = createSignal(50);
  const [hue, setHue] = createSignal(0);
  const [saturation, setSaturation] = createSignal(100);
  const [colorTemperature, setColorTemperature] = createSignal(4000);
  const [selectedRoomKey, setSelectedRoomKey] = createSignal(MATTER_ALL_ROOMS);
  const [deviceBusy, setDeviceBusy] = createSignal(false);
  const [runModeDraft, setRunModeDraft] = createSignal<number>();
  const [cleanModeDraft, setCleanModeDraft] = createSignal<number>();
  const optimistic = new Set<string>();
  let pollTimer: number | undefined;
  let refreshTimer: number | undefined;
  let unsubscribeMatterEvents: (() => void) | undefined;

  const roomFilters = createMemo(() => matterRoomFilters(devices()));
  const selectedRoomFilter = createMemo(() =>
    roomFilters().find((filter) => filter.key === selectedRoomKey()),
  );
  const visibleDevices = createMemo(() =>
    filterMatterDevices(devices(), selectedRoomFilter()),
  );

  const applyReportedControl = (property: MatterProperty) => {
    if (property.cluster === 'on_off' && property.key === 'on' &&
        typeof property.value === 'boolean') {
      setPowerOn(property.value);
    } else if (property.cluster === 'level_control' && property.key === 'level_percent' &&
               typeof property.value === 'number') {
      setBrightness(property.value);
    } else if (property.cluster === 'color_control' && typeof property.value === 'number') {
      if (property.key === 'hue_degrees') setHue(property.value);
      else if (property.key === 'saturation_percent') setSaturation(property.value);
      else if (property.key === 'color_temperature_kelvin') setColorTemperature(property.value);
    }
  };

  const applySelectedDevice = (detail: MatterDeviceDetail) => {
    const previous = selected();
    if (previous?.device_id !== detail.device_id) {
      optimistic.clear();
      setRunModeDraft(undefined);
      setCleanModeDraft(undefined);
    }
    setSelected(detail);
    setName(detail.name);
    setRoom(detail.room);
    for (const property of detail.properties) {
      const key = propertyKey(property);
      if (optimistic.has(key)) {
        const oldValue = previous?.properties.find((item) => propertyKey(item) === key)?.value;
        if (Object.is(oldValue, property.value)) continue;
        optimistic.delete(key);
      }
      applyReportedControl(property);
    }
  };

  const waitForOperation = async (requestId: number) => {
    for (let attempt = 0; attempt < 60; attempt += 1) {
      const operation = await fetchMatterOperation(requestId);
      if (!operation.active) {
        const error = matterOperationError(operation);
        if (error) throw new Error(error);
        return;
      }
      await new Promise((resolve) => window.setTimeout(resolve, 250));
    }
    throw new Error('Matter operation timed out');
  };

  const refreshDevices = async () => {
    setLoading(true);
    try {
      const next = await fetchMatterDevices();
      setDevices(next);
      const current = selected();
      if (current && next.some((device) => device.device_id === current.device_id)) {
        applySelectedDevice(await fetchMatterDevice(current.device_id));
      } else if (current) {
        optimistic.clear();
        setSelected(undefined);
      }
    } catch (error) {
      reportError(error);
    } finally {
      setLoading(false);
    }
  };

  const scheduleRefresh = () => {
    if (refreshTimer !== undefined) return;
    refreshTimer = window.setTimeout(() => {
      refreshTimer = undefined;
      void refreshDevices();
    }, 75);
  };

  const handleMatterEvent = (event: MatterEvent) => {
    if (event.type !== 'properties_changed') {
      scheduleRefresh();
      return;
    }
    const current = selected();
    if (!current) return;
    const changes = event.changes.filter(
      (change) => change.device_id === current.device_id,
    );
    if (!changes.length) return;

    setSelected({
      ...current,
      properties: current.properties.map((property) => {
        const change = changes.find(
          (item) =>
            item.endpoint === property.endpoint &&
            item.cluster === property.cluster &&
            item.property === property.key,
        );
        return change ? { ...property, value: change.value } : property;
      }),
    });
    for (const change of changes) {
      const property = {
        endpoint: change.endpoint,
        cluster: change.cluster,
        key: change.property,
        value: change.value,
      };
      optimistic.delete(propertyKey(property));
      applyReportedControl(property);
    }
  };

  const openDevice = async (device: MatterDevice) => {
    try {
      const detail = await fetchMatterDevice(device.device_id);
      applySelectedDevice(detail);
    } catch (error) {
      reportError(error);
    }
  };

  const pollCommission = async () => {
    try {
      const status = await pollMatterCommission();
      setCommissioning(status.active);
      if (!status.active && pollTimer !== undefined) {
        window.clearInterval(pollTimer);
        pollTimer = undefined;
        if (status.result === 'ESP_OK') await refreshDevices();
        else pushToast(`Commissioning finished: ${status.result}`, 'error');
      }
    } catch (error) {
      reportError(error);
    }
  };

  onMount(() => {
    unsubscribeMatterEvents = subscribeMatterEvents(handleMatterEvent, scheduleRefresh);
    void refreshDevices();
    void pollCommission();
  });
  onCleanup(() => {
    if (pollTimer !== undefined) window.clearInterval(pollTimer);
    if (refreshTimer !== undefined) window.clearTimeout(refreshTimer);
    unsubscribeMatterEvents?.();
  });
  createEffect(() => {
    setSelectedRoomKey((current) => reconcileMatterRoomFilter(current, roomFilters()));
  });
  createEffect(() => {
    if (commissioning() && pollTimer === undefined) {
      pollTimer = window.setInterval(() => void pollCommission(), 1000);
    }
  });

  const commission = async () => {
    try {
      await startMatterCommission(setupCode().trim());
      setCommissioning(true);
      pushToast('Commissioning started', 'success');
    } catch (error) {
      reportError(error);
    }
  };

  const cancelCommission = async () => {
    try {
      await cancelMatterCommission();
      setCommissioning(false);
    } catch (error) {
      reportError(error);
    }
  };

  const saveIdentity = async () => {
    const device = selected();
    if (!device || deviceBusy()) return;
    let identity;
    try {
      identity = normalizeMatterIdentity(name(), room());
    } catch (error) {
      reportError(error);
      return;
    }
    setDeviceBusy(true);
    try {
      await updateMatterDeviceIdentity(device.device_id, identity.name, identity.room);
      await refreshDevices();
      pushToast('Device updated', 'success');
    } catch (error) {
      reportError(error);
    } finally {
      setDeviceBusy(false);
    }
  };

  const removeDevice = async () => {
    const device = selected();
    if (!device || deviceBusy()) return;
    if (!window.confirm('Remove this device? Other devices on the same Matter node may also be removed.')) {
      return;
    }
    setDeviceBusy(true);
    try {
      const operation = await removeMatterDevice(device.device_id);
      if (!operation.request_id) throw new Error('Removal did not return a request identifier');
      await waitForOperation(operation.request_id);
      setSelected(undefined);
      await refreshDevices();
      pushToast('Device removed', 'success');
    } catch (error) {
      reportError(error);
    } finally {
      setDeviceBusy(false);
    }
  };

  const invoke = async (
    action: MatterAction,
    args: Record<string, boolean | number | string>,
    property: MatterProperty,
  ) => {
    const device = selected();
    if (!device || deviceBusy()) return;
    const key = propertyKey(property);
    optimistic.add(key);
    setDeviceBusy(true);
    try {
      const operation = await invokeMatterAction(
        device.device_id,
        action.endpoint,
        action.cluster,
        action.key,
        args,
      );
      if (!operation.request_id) throw new Error('Command did not return a request identifier');
      await waitForOperation(operation.request_id);
      pushToast('Command accepted', 'success');
    } catch (error) {
      if (selected()?.device_id === device.device_id) {
        optimistic.delete(key);
        const reported = selected()?.properties.find((item) => propertyKey(item) === key);
        if (reported) applyReportedControl(reported);
      }
      reportError(error);
    } finally {
      setDeviceBusy(false);
    }
  };

  const startRvc = async (run: MatterAction, runMode: number,
                          clean: MatterAction, cleanMode: number) => {
    const device = selected();
    if (!device || deviceBusy()) return;
    setDeviceBusy(true);
    try {
      // Clean Mode must finish before Run Mode starts the activity.
      for (const [action, mode] of [[clean, cleanMode], [run, runMode]] as const) {
        const operation = await invokeMatterAction(device.device_id, action.endpoint,
          action.cluster, action.key, { mode });
        if (!operation.request_id) throw new Error('Command did not return a request identifier');
        await waitForOperation(operation.request_id);
      }
      pushToast('RVC started', 'success');
    } catch (error) {
      reportError(error);
    } finally {
      setDeviceBusy(false);
    }
  };

  return (
    <div class="space-y-5">
      <PageHeader title="Matter Fabric" description="Commission, control, and manage semantic Matter devices." />
      <TabShell>
        <Section title="Commission a device" note="QR payloads and manual setup codes support BLE, on-network, and available Thread commissioning.">
          <div class="grid gap-3 sm:grid-cols-[1fr_auto] sm:items-end">
            <TextInput
              label="Setup code or QR payload"
              value={setupCode()}
              onInput={(event) => setSetupCode(event.currentTarget.value)}
              disabled={commissioning()}
            />
            <Show
              when={commissioning()}
              fallback={<Button variant="primary" onClick={() => void commission()} disabled={!setupCode().trim()}>Commission</Button>}
            >
              <Button onClick={() => void cancelCommission()}>Cancel</Button>
            </Show>
          </div>
        </Section>

        <Section title="Devices">
          <div class="mb-4 flex items-center gap-3">
            <div
              class="flex min-w-0 flex-1 gap-2 overflow-x-auto pb-1"
              aria-label="Filter devices by room"
            >
              <For each={roomFilters()}>
                {(filter) => (
                  <button
                    type="button"
                    aria-pressed={selectedRoomKey() === filter.key}
                    class={`shrink-0 rounded-full border px-4 py-2 text-sm font-medium transition ${
                      selectedRoomKey() === filter.key
                        ? 'border-[var(--color-accent)] bg-[var(--color-accent)] text-white'
                        : 'border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)] text-[var(--color-text-muted)] hover:border-[var(--color-border-strong)] hover:text-[var(--color-text-primary)]'
                    }`}
                    onClick={() => setSelectedRoomKey(filter.key)}
                  >
                    {filter.label}
                  </button>
                )}
              </For>
            </div>
            <Button onClick={() => void refreshDevices()} disabled={loading()}>Refresh</Button>
          </div>
          <Show when={!loading() || devices().length > 0} fallback={<p class="text-sm text-[var(--color-text-muted)]">Loading…</p>}>
            <div class="grid gap-3 md:grid-cols-2">
              <For each={visibleDevices()} fallback={<p class="text-sm text-[var(--color-text-muted)]">No devices in this room.</p>}>
                {(device) => (
                  <button
                    type="button"
                    class="rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)] p-4 text-left transition hover:border-[var(--color-border-strong)]"
                    onClick={() => void openDevice(device)}
                  >
                    <div class="flex items-center justify-between gap-2">
                      <strong>{device.name}</strong>
                      <span class={device.online ? 'text-emerald-400' : 'text-[var(--color-text-muted)]'}>
                        {device.online ? 'Online' : 'Offline'}
                      </span>
                    </div>
                    <p class="mt-1 text-xs text-[var(--color-text-muted)]">
                      {device.type}{device.room ? ` · ${device.room}` : ''}
                    </p>
                  </button>
                )}
              </For>
            </div>
          </Show>
        </Section>

        <Show when={selected()?.device_id} keyed>
          {(_deviceId) => {
            const device = () => selected()!;
            const power = () => findMatterAction(device().actions, 'on_off', 'set');
            const brightnessAction = () => findMatterAction(
              device().actions,
              'level_control',
              'set_level',
            );
            const colorAction = () => findMatterAction(
              device().actions,
              'color_control',
              'set_color',
            );
            const temperatureAction = () => findMatterAction(
              device().actions,
              'color_control',
              'set_color_temperature',
            );
            const temperatureControl = createMemo(() => {
              const action = temperatureAction();
              const argument = action?.arguments.color_temperature_kelvin;
              const minimum = argument?.min;
              const maximum = argument?.max;
              return Number.isInteger(minimum) && Number.isInteger(maximum) &&
                (minimum as number) >= 1 && (maximum as number) >= (minimum as number)
                ? { action: action!, range: { minimum: minimum as number, maximum: maximum as number } }
                : undefined;
            });
            const powerProperty = () => findProperty(device(), 'on_off', 'on');
            const brightnessProperty = () => findProperty(
              device(),
              'level_control',
              'level_percent',
            );
            const hueProperty = () => findProperty(device(), 'color_control', 'hue_degrees');
            const saturationProperty = () => findProperty(
              device(),
              'color_control',
              'saturation_percent',
            );
            const temperatureProperty = () => findProperty(
              device(),
              'color_control',
              'color_temperature_kelvin',
            );
            const thermostatMode = () => findMatterAction(device().actions, 'thermostat', 'set_mode');
            const runMode = () => findMatterAction(device().actions, 'rvc_run_mode', 'set_mode');
            const cleanMode = () => findMatterAction(device().actions, 'rvc_clean_mode', 'set_mode');
            const rvcState = () => findProperty(device(), 'rvc_operational_state', 'state');
            const rvcPhase = () => typeof rvcState()?.value === 'string'
              ? rvcState()!.value as string : '';
            const rvcChoices = (action: MatterAction | undefined) =>
              (action?.arguments.mode?.enum ?? []).filter(
                (choice): choice is { value: number; label: string } =>
                  typeof choice === 'object' && Number.isInteger(choice.value),
              );
            const rvcMode = (cluster: string) => findProperty(device(), cluster, 'mode');
            const canSelectRvcMode = () => ['stopped', 'docked', 'charging'].includes(rvcPhase());
            const selectedRunMode = () => {
              const reported = rvcMode('rvc_run_mode')?.value;
              if (!canSelectRvcMode() && typeof reported === 'number' && reported !== 0) {
                return reported;
              }
              return runModeDraft() ?? (typeof reported === 'number' && reported !== 0
                ? reported : rvcChoices(runMode()).find((mode) => mode.value !== 0)?.value);
            };
            const selectedCleanMode = () => {
              const reported = rvcMode('rvc_clean_mode')?.value;
              if (!canSelectRvcMode() && typeof reported === 'number') return reported;
              return cleanModeDraft() ?? (typeof reported === 'number' ? reported : undefined);
            };
            return (
              <Section title={`Device: ${device().name}`}>
                <div class="space-y-5">
                  <div class="grid gap-3 sm:grid-cols-2">
                    <TextInput label="Name" value={name()} onInput={(event) => setName(event.currentTarget.value)} disabled={deviceBusy()} />
                    <TextInput label="Room" value={room()} onInput={(event) => setRoom(event.currentTarget.value)} disabled={deviceBusy()} />
                  </div>
                  <div class="flex flex-wrap gap-2">
                    <Button variant="primary" onClick={() => void saveIdentity()} disabled={deviceBusy() || !name().trim()}>Save</Button>
                    <Button variant="danger-ghost" onClick={() => void removeDevice()} disabled={deviceBusy()}>Remove</Button>
                  </div>

                  <Show when={power() || brightnessAction() || colorAction() || temperatureAction()}>
                    <div class="space-y-3 rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] p-4">
                      <Show when={power()}>
                        {(action) => (
                          <Button
                            disabled={deviceBusy() || !powerProperty()}
                            onClick={() => {
                              const property = powerProperty();
                              if (!property) return;
                              const currentlyOn = powerOn();
                              setPowerOn(!currentlyOn);
                              void invoke(action(), powerArguments(currentlyOn), property);
                            }}
                          >
                            {powerOn() ? 'Turn off' : 'Turn on'}
                          </Button>
                        )}
                      </Show>
                      <Show when={brightnessAction()}>
                        {(action) => (
                          <label class="block text-sm">
                            Brightness: {brightness()}%
                            <input class="mt-2 w-full" type="range" min="1" max="100" value={brightness()}
                              disabled={deviceBusy() || !brightnessProperty()}
                              onInput={(event) => {
                                const property = brightnessProperty();
                                if (property) optimistic.add(propertyKey(property));
                                setBrightness(Number(event.currentTarget.value));
                              }}
                              onChange={() => {
                                const property = brightnessProperty();
                                if (property) void invoke(
                                  action(), brightnessArguments(brightness()), property,
                                );
                              }} />
                          </label>
                        )}
                      </Show>
                      <Show when={temperatureControl()}>
                        {(control) => (
                          <div class="rounded-[var(--radius-sm)] bg-[var(--color-bg-surface)] p-3">
                            <div class="mb-2 text-xs font-semibold uppercase tracking-wide text-[var(--color-text-muted)]">White</div>
                            <label class="block text-sm">
                              Color temperature: {colorTemperature()} K
                              <input class="mt-2 w-full" type="range"
                                min={control().range.minimum}
                                max={control().range.maximum}
                                step="1" value={colorTemperature()}
                                disabled={deviceBusy() || !temperatureProperty()}
                                onInput={(event) => {
                                  const property = temperatureProperty();
                                  if (property) optimistic.add(propertyKey(property));
                                  setColorTemperature(Number(event.currentTarget.value));
                                }}
                                onChange={() => {
                                  const property = temperatureProperty();
                                  if (!property) return;
                                  const { action, range } = control();
                                  void invoke(
                                    action,
                                    colorTemperatureArguments(
                                      colorTemperature(), range.minimum, range.maximum,
                                    ),
                                    property,
                                  );
                                }} />
                            </label>
                          </div>
                        )}
                      </Show>
                      <Show when={colorAction()}>
                        {(action) => (
                          <div class="space-y-3 rounded-[var(--radius-sm)] bg-[var(--color-bg-surface)] p-3">
                            <div class="text-xs font-semibold uppercase tracking-wide text-[var(--color-text-muted)]">Color</div>
                            <label class="block text-sm">
                              Hue: {hue()}°
                              <input
                                class="mt-2 w-full"
                                style={{ 'accent-color': `hsl(${hue()} 100% 50%)` }}
                                type="range"
                                min="0"
                                max="359"
                                value={hue()}
                                disabled={deviceBusy() || !hueProperty() || !saturationProperty()}
                                onInput={(event) => {
                                  const property = hueProperty();
                                  if (property) optimistic.add(propertyKey(property));
                                  setHue(Number(event.currentTarget.value));
                                }}
                                onChange={() => {
                                  const property = hueProperty();
                                  if (property) void invoke(
                                    action(), colorArguments(hue(), saturation()), property,
                                  );
                                }}
                              />
                            </label>
                            <label class="block text-sm">
                              Saturation: {saturation()}%
                              <input
                                class="mt-2 w-full"
                                type="range"
                                min="0"
                                max="100"
                                value={saturation()}
                                disabled={deviceBusy() || !hueProperty() || !saturationProperty()}
                                onInput={(event) => {
                                  const property = saturationProperty();
                                  if (property) optimistic.add(propertyKey(property));
                                  setSaturation(Number(event.currentTarget.value));
                                }}
                                onChange={() => {
                                  const property = saturationProperty();
                                  if (property) void invoke(
                                    action(), colorArguments(hue(), saturation()), property,
                                  );
                                }}
                              />
                            </label>
                          </div>
                        )}
                      </Show>
                    </div>
                  </Show>

                  <Show when={thermostatMode() || findProperty(device(), 'thermostat', 'local_temperature_c')}>
                    <div class="space-y-3 rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] p-4">
                      <h3 class="font-semibold">Thermostat</h3>
                      <p class="text-sm">Current temperature: {
                        typeof findProperty(device(), 'thermostat', 'local_temperature_c')?.value === 'number'
                          ? `${findProperty(device(), 'thermostat', 'local_temperature_c')!.value} °C`
                          : 'Unknown'
                      }</p>
                      <Show when={thermostatMode()}>
                        {(action) => (
                          <SelectInput label="System mode"
                            value={String(findProperty(device(), 'thermostat', 'system_mode')?.value ?? '')}
                            disabled={!device().online || deviceBusy() || !findProperty(device(), 'thermostat', 'system_mode')}
                            onChange={(event) => {
                              const property = findProperty(device(), 'thermostat', 'system_mode');
                              if (property) {
                                const mode = event.currentTarget.value;
                                event.currentTarget.value = String(property.value ?? '');
                                void invoke(action(), { mode }, property);
                              }
                            }}>
                            <option value="" disabled>Unknown</option>
                            <For each={(action().arguments.mode?.enum ?? []).filter(
                              (mode): mode is string => typeof mode === 'string',
                            )}>{(mode) => <option value={mode}>{mode.charAt(0).toUpperCase() + mode.slice(1)}</option>}</For>
                          </SelectInput>
                        )}
                      </Show>
                      <div class="grid gap-3 sm:grid-cols-2">
                        <For each={[
                          { key: 'heating_setpoint_c', action: 'set_heating_setpoint', label: 'Heat target' },
                          { key: 'cooling_setpoint_c', action: 'set_cooling_setpoint', label: 'Cool target' },
                        ]}>
                          {(target) => {
                            const action = () => findMatterAction(device().actions, 'thermostat', target.action);
                            const property = () => findProperty(device(), 'thermostat', target.key);
                            const range = () => action()?.arguments.temperature_c;
                            return <Show when={property()}>
                              <TextInput type="number" label={`${target.label} (°C)`}
                                min={range()?.min} max={range()?.max} step="0.01"
                                value={typeof property()?.value === 'number' ? String(property()!.value) : ''}
                                disabled={!device().online || deviceBusy() || !action() ||
                                  !Number.isFinite(range()?.min) || !Number.isFinite(range()?.max)}
                                onChange={(event) => {
                                  const value = Number(event.currentTarget.value);
                                  const limits = range();
                                  const reported = property();
                                  if (action() && reported && limits && event.currentTarget.value !== '' &&
                                      Number.isFinite(value) && limits.min !== undefined && limits.max !== undefined &&
                                      value >= limits.min && value <= limits.max) {
                                    event.currentTarget.value = String(reported.value ?? '');
                                    void invoke(action()!, { temperature_c: value }, reported);
                                  } else {
                                    event.currentTarget.value = String(reported?.value ?? '');
                                  }
                                }} />
                            </Show>;
                          }}
                        </For>
                      </div>
                    </div>
                  </Show>

                  <Show when={runMode() || rvcState()}>
                    <div class="space-y-3 rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] p-4">
                      <h3 class="font-semibold">Robot vacuum</h3>
                      <p class="text-sm">Status: {rvcPhase().replaceAll('_', ' ') || 'Unknown'}</p>
                      <Show when={runMode()}>
                        {(action) => (
                          <SelectInput label="Run Mode"
                            value={String(selectedRunMode() ?? '')}
                            disabled={!device().online || deviceBusy() || !rvcMode('rvc_run_mode') ||
                              !canSelectRvcMode()}
                            onChange={(event) => setRunModeDraft(Number(event.currentTarget.value))}>
                            <option value="" disabled>Select activity</option>
                            <For each={rvcChoices(action()).filter((mode) => mode.value !== 0)}>
                              {(mode) => <option value={mode.value}>{mode.label}</option>}
                            </For>
                          </SelectInput>
                        )}
                      </Show>
                      <Show when={cleanMode()}>
                        {(action) => (
                          <SelectInput label="Clean Mode"
                            value={String(selectedCleanMode() ?? '')}
                            disabled={!device().online || deviceBusy() || !rvcMode('rvc_clean_mode') ||
                              !canSelectRvcMode()}
                            onChange={(event) => setCleanModeDraft(Number(event.currentTarget.value))}>
                            <option value="" disabled>Unknown</option>
                            <For each={rvcChoices(action())}>
                              {(mode) => <option value={mode.value}>{mode.label}</option>}
                            </For>
                          </SelectInput>
                        )}
                      </Show>
                      <div class="flex gap-2">
                        <Show when={canSelectRvcMode()}>
                          <Button variant="primary"
                            disabled={!device().online || deviceBusy() || !runMode() || !cleanMode() ||
                              selectedRunMode() === undefined || selectedCleanMode() === undefined}
                            onClick={() => {
                              const run = runMode();
                              const clean = cleanMode();
                              const runChoice = selectedRunMode();
                              const cleanChoice = selectedCleanMode();
                              if (run && clean && runChoice !== undefined && cleanChoice !== undefined) {
                                void startRvc(run, runChoice, clean, cleanChoice);
                              }
                            }}>Start</Button>
                        </Show>
                        <Show when={rvcPhase() === 'running' || rvcPhase() === 'paused'}>
                          <Button disabled={!device().online || deviceBusy() ||
                            !findMatterAction(device().actions, 'rvc_operational_state',
                              rvcPhase() === 'paused' ? 'resume' : 'pause')}
                            onClick={() => {
                              const action = findMatterAction(device().actions, 'rvc_operational_state',
                                rvcPhase() === 'paused' ? 'resume' : 'pause');
                              const property = rvcState();
                              if (action && property) void invoke(action, {}, property);
                            }}>
                            {rvcPhase() === 'paused' ? 'Resume' : 'Pause'}
                          </Button>
                        </Show>
                        <Show when={findMatterAction(device().actions, 'rvc_operational_state', 'go_home')}>
                          {(action) => (
                            <Button disabled={!device().online || deviceBusy() ||
                              !['running', 'paused', 'stopped'].includes(rvcPhase())}
                              onClick={() => {
                                const property = rvcState();
                                if (property) void invoke(action(), {}, property);
                              }}>Go home</Button>
                          )}
                        </Show>
                      </div>
                    </div>
                  </Show>

                  <div class="grid gap-2 sm:grid-cols-2">
                    <For each={device().properties}>
                      {(property) => (
                        <div class="rounded-[var(--radius-sm)] bg-[var(--color-bg-surface)] px-3 py-2 text-sm">
                          <span class="text-[var(--color-text-muted)]">
                            {property.key}
                          </span>
                          <span class="float-right font-mono">{JSON.stringify(property.value)}</span>
                        </div>
                      )}
                    </For>
                  </div>
                </div>
              </Section>
            );
          }}
        </Show>
      </TabShell>
    </div>
  );
};
