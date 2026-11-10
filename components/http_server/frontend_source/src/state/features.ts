import type { AppConfig, CapabilityItem, StatusInfo } from '../api/client';

const BASE_ROUTES = new Set(['status', 'basic', 'files']);
const IM_CAPABILITIES = {
  wechat: 'cap_im_wechat',
  feishu: 'cap_im_feishu',
  qq: 'cap_im_qq',
  telegram: 'cap_im_tg',
} as const;

export type ImPlatform = keyof typeof IM_CAPABILITIES;

export function capabilitySupported(items: CapabilityItem[], id: string): boolean {
  return items.some((item) => item.group_id === id);
}

export function capabilityEnabled(items: CapabilityItem[], id: string, selection: string): boolean {
  if (!capabilitySupported(items, id)) return false;
  return !selection.trim() || selection.split(',').some((token) => token.trim() === id);
}

export function imPlatformAvailable(platform: ImPlatform, items: CapabilityItem[]): boolean {
  return capabilitySupported(items, IM_CAPABILITIES[platform]);
}

export function routeNeedsCapabilities(route: string): boolean {
  return route === 'im' || route === 'webreq';
}

export function routeAvailable(
  route: string,
  status: StatusInfo | null,
  items: CapabilityItem[] = [],
): boolean {
  if (BASE_ROUTES.has(route)) return true;
  if (route === 'matter') return status?.matter_enabled === true;
  if (status?.claw_enabled !== true) return false;
  if (route === 'webim') return status.webim_supported === true;
  if (route === 'asr') return status.asr_supported === true;
  if (route === 'memory') return status.memory_supported === true;
  if (route === 'im')
    return Object.values(IM_CAPABILITIES).some((id) => capabilitySupported(items, id));
  if (route === 'webreq') {
    return (
      capabilitySupported(items, 'cap_web_search') || capabilitySupported(items, 'cap_http_request')
    );
  }
  return true;
}

export function wechatLoginAvailable(status: StatusInfo | null): boolean {
  return status?.claw_enabled === true && status.wechat_login_supported === true;
}

export function restartAvailable(status: StatusInfo | null): boolean {
  return status?.restart_supported === true;
}

/** Match the application's field filtering without discarding stored settings. */
export function filterCapabilityConfig(
  patch: Partial<AppConfig>,
  items: CapabilityItem[],
): Partial<AppConfig> {
  return Object.fromEntries(
    Object.entries(patch).filter(([field]) => {
      for (const platform of Object.keys(IM_CAPABILITIES) as ImPlatform[]) {
        const prefix = platform === 'telegram' ? 'tg_' : platform + '_';
        if (field.startsWith(prefix)) return imPlatformAvailable(platform, items);
      }
      if (field === 'search_brave_key' || field === 'search_tavily_key') {
        return capabilitySupported(items, 'cap_web_search');
      }
      if (field === 'search_http_allowlist') return capabilitySupported(items, 'cap_http_request');
      if (field === 'enabled_lua_modules') return capabilitySupported(items, 'cap_lua');
      return true;
    }),
  );
}
