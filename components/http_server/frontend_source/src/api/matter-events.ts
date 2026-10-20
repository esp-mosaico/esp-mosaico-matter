export type MatterPropertyChange = {
  device_id: number;
  endpoint: number;
  cluster: string;
  property: string;
  previous_value: unknown;
  value: unknown;
};

export type MatterEvent =
  | { type: 'properties_changed'; changes: MatterPropertyChange[] }
  | { type: 'devices_changed' }
  | { type: 'resync' };

export function parseMatterEvent(data: string): MatterEvent | undefined {
  let value: unknown;
  try {
    value = JSON.parse(data);
  } catch {
    return undefined;
  }
  if (!value || typeof value !== 'object' || !('type' in value)) return undefined;
  if (value.type === 'devices_changed' || value.type === 'resync') {
    return { type: value.type };
  }
  if (
    value.type !== 'properties_changed' ||
    !('changes' in value) ||
    !Array.isArray(value.changes)
  ) {
    return undefined;
  }

  const changes: MatterPropertyChange[] = [];
  for (const change of value.changes) {
    if (
      !change ||
      typeof change !== 'object' ||
      !('device_id' in change) ||
      typeof change.device_id !== 'number' ||
      !Number.isInteger(change.device_id) ||
      !('endpoint' in change) ||
      typeof change.endpoint !== 'number' ||
      !Number.isInteger(change.endpoint) ||
      !('cluster' in change) ||
      typeof change.cluster !== 'string' ||
      !('property' in change) ||
      typeof change.property !== 'string' ||
      !('previous_value' in change) ||
      !('value' in change)
    ) {
      return undefined;
    }
    changes.push(change as MatterPropertyChange);
  }
  return { type: 'properties_changed', changes };
}
