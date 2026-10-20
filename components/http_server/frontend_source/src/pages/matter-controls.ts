export type MatterActionCoordinate = {
  endpoint: number;
  cluster: string;
  key: string;
};
export type MatterRoomDevice = { room: string };
export type MatterRoomFilter = {
  key: string;
  label: string;
  room?: string;
  unassigned?: boolean;
};
export type MatterOperationResult = { result: string };
export type MatterPropertyCoordinate = {
  endpoint: number;
  cluster: string;
  key: string;
};
export type MatterPropertySnapshot = {
  properties: Array<MatterPropertyCoordinate & { value: unknown }>;
};

export const MATTER_REPORT_WAIT_ATTEMPTS = 20;
export const MATTER_REPORT_WAIT_INTERVAL_MS = 250;

export const MATTER_ALL_ROOMS = 'all';
const MATTER_ROOM_PREFIX = 'room:';
const MATTER_OTHER_ROOM = 'other';

export function normalizeMatterIdentity(name: string, room: string) {
  const normalizedName = name.trim();
  if (!normalizedName) throw new RangeError('device name must not be empty');
  return { name: normalizedName, room: room.trim() };
}

export function matterOperationError(operation: MatterOperationResult): string | undefined {
  if (operation.result !== 'ESP_OK') return operation.result;
  return undefined;
}

export function findMatterAction<T extends MatterActionCoordinate>(
  actions: T[],
  cluster: string,
  key: string,
): T | undefined {
  return actions.find((action) => action.cluster === cluster && action.key === key);
}

export const powerArguments = (currentlyOn: boolean) => ({ on: !currentlyOn });

export function brightnessArguments(brightnessPercent: number) {
  if (!Number.isInteger(brightnessPercent) || brightnessPercent < 1 || brightnessPercent > 100) {
    throw new RangeError('brightness must be a whole percentage from 1 to 100');
  }
  return { level_percent: brightnessPercent };
}

export function colorArguments(hueDegrees: number, saturationPercent: number) {
  if (!Number.isInteger(hueDegrees) || hueDegrees < 0 || hueDegrees > 359) {
    throw new RangeError('hue must be a whole value from 0 to 359 degrees');
  }
  if (
    !Number.isInteger(saturationPercent) ||
    saturationPercent < 0 ||
    saturationPercent > 100
  ) {
    throw new RangeError('saturation must be a whole percentage from 0 to 100');
  }
  return { hue_degrees: hueDegrees, saturation_percent: saturationPercent };
}

export function matterRoomFilters(devices: MatterRoomDevice[]): MatterRoomFilter[] {
  const rooms = [...new Set(devices.map((device) => device.room.trim()).filter(Boolean))].sort(
    (left, right) => left.localeCompare(right),
  );
  const filters: MatterRoomFilter[] = [
    { key: MATTER_ALL_ROOMS, label: 'All' },
    ...rooms.map((room) => ({
      key: `${MATTER_ROOM_PREFIX}${encodeURIComponent(room)}`,
      label: room,
      room,
    })),
  ];
  if (devices.some((device) => !device.room.trim())) {
    filters.push({ key: MATTER_OTHER_ROOM, label: 'Other', unassigned: true });
  }
  return filters;
}

export function reconcileMatterRoomFilter(
  selectedKey: string,
  filters: MatterRoomFilter[],
): string {
  return filters.some((filter) => filter.key === selectedKey) ? selectedKey : MATTER_ALL_ROOMS;
}

export function filterMatterDevices<T extends MatterRoomDevice>(
  devices: T[],
  filter: MatterRoomFilter | undefined,
): T[] {
  if (!filter || filter.key === MATTER_ALL_ROOMS) return devices;
  if (filter.unassigned) return devices.filter((device) => !device.room.trim());
  return devices.filter((device) => device.room.trim() === filter.room);
}

export function canonicalColorTemperatureKelvin(colorTemperatureKelvin: number) {
  if (!Number.isInteger(colorTemperatureKelvin) || colorTemperatureKelvin < 1) {
    throw new RangeError('color temperature must be a positive whole Kelvin value');
  }
  const mireds = Math.round(1_000_000 / colorTemperatureKelvin);
  return Math.round(1_000_000 / mireds);
}

export async function waitForMatterReportedValue<T extends MatterPropertySnapshot>(
  read: () => Promise<T>,
  property: MatterPropertyCoordinate,
  expectedValue: boolean | number,
  publish: (snapshot: T) => void,
  attempts = MATTER_REPORT_WAIT_ATTEMPTS,
  delay: (milliseconds: number) => Promise<void> =
    (milliseconds) => new Promise((resolve) => globalThis.setTimeout(resolve, milliseconds)),
): Promise<void> {
  for (let attempt = 0; attempt < attempts; attempt += 1) {
    const snapshot = await read();
    publish(snapshot);
    const reported = snapshot.properties.find(
      (item) =>
        item.endpoint === property.endpoint &&
        item.cluster === property.cluster &&
        item.key === property.key,
    );
    if (Object.is(reported?.value, expectedValue)) return;
    if (attempt + 1 < attempts) await delay(MATTER_REPORT_WAIT_INTERVAL_MS);
  }
  throw new Error('Command completed, but the reported device state did not update');
}

export function colorTemperatureArguments(
  colorTemperatureKelvin: number,
  minimumKelvin: number,
  maximumKelvin: number,
) {
  if (
    !Number.isInteger(minimumKelvin) ||
    !Number.isInteger(maximumKelvin) ||
    minimumKelvin < 1 ||
    maximumKelvin < minimumKelvin
  ) {
    throw new RangeError('color temperature range is invalid');
  }
  if (
    !Number.isInteger(colorTemperatureKelvin) ||
    colorTemperatureKelvin < minimumKelvin ||
    colorTemperatureKelvin > maximumKelvin
  ) {
    throw new RangeError(
      `color temperature must be a whole value from ${minimumKelvin} K to ${maximumKelvin} K`,
    );
  }
  return { color_temperature_kelvin: colorTemperatureKelvin };
}
