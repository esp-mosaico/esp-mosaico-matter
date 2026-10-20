import {
  Activity,
  Blocks,
  Bot,
  DatabaseZap,
  Folder,
  MessageSquareCode,
  MessagesSquare,
  Mic,
  Radio,
  Search,
  Settings,
  SquareFunction,
  WifiPen,
} from 'lucide-solid';
import { createMemo, createSignal, For, Show, type Component } from 'solid-js';
import { t } from '../../i18n';
import { appStatus } from '../../state/config';
import { isDirty, type TabId } from '../../state/dirty';
import { Logo, StatusSummary } from './StatusBar';

const iconClass = 'w-4 h-4 shrink-0';

/**
 * ESP-Claw claw glyph — the three rounded quadrants of the mark, lifted from
 * esp-mosaico-claw/third-party/esp-claw/docs/src/assets/logos/logo.svg.
 */
const ClawGlyph: Component<{ class?: string }> = (props) => (
  <svg class={props.class} viewBox="0 0 165.4 191.26" fill="#e8362d" aria-hidden="true">
    <path d="M165.38 113v.06l-.04.54v.04c-1.29 20.81-10.26 39.52-24.09 53.36-15 15-35.69 24.26-58.57 24.26-9.69 0-18.97-1.66-27.6-4.71C24.43 175.72 2.07 147.36.03 113.53c-.01-.15-.03-.32-.03-.47 0-1.27.52-2.42 1.36-3.26s1.98-1.36 3.26-1.36h156.15c1.29 0 2.43.52 3.27 1.36.82.82 1.34 1.96 1.36 3.2zM55.07 37.15v52.87c0 1.27-.52 2.42-1.36 3.26-.82.84-1.98 1.36-3.26 1.36H6.65c-2.54 0-4.61-2.07-4.61-4.61 0-.31.03-.6.08-.88l.13-.49a82.4 82.4 0 0 1 21.86-38.77c7.03-7.03 15.3-12.8 24.47-16.95.03-.01.04-.01.07-.03.54-.24 1.16-.36 1.8-.36 1.27 0 2.43.52 3.26 1.36.84.82 1.36 1.98 1.36 3.26zm108.26 52.87c0 1.27-.52 2.42-1.36 3.26s-2 1.36-3.27 1.36H87.28c-2.54 0-4.61-2.07-4.61-4.61V4.61c0-1.27.52-2.43 1.36-3.27a4.645 4.645 0 0 1 5.06-.98c.14.06.27.13.41.2 14.45 6.65 27.52 15.83 38.64 26.96 15.44 15.44 27.14 34.65 33.58 56.13.03.06.04.13.06.18.46 1.5.88 3.01 1.27 4.53.03.1.06.2.07.31.01.03.01.06.03.08.03.13.07.25.08.38.07.29.1.6.1.91z" />
  </svg>
);

const IconStatus: Component = () => <Activity class={iconClass} />;
const IconGear: Component = () => <Settings class={iconClass} />;
const IconWifi: Component = () => <WifiPen class={iconClass} />;
const IconLlm: Component = () => <Bot class={iconClass} />;
const IconAsr: Component = () => <Mic class={iconClass} />;
const IconIm: Component = () => <MessageSquareCode class={iconClass} />;
const IconSearch: Component = () => <Search class={iconClass} />;
const IconMemory: Component = () => <DatabaseZap class={iconClass} />;
const IconCaps: Component = () => <Blocks class={iconClass} />;
const IconSkills: Component = () => <SquareFunction class={iconClass} />;
const IconFiles: Component = () => <Folder class={iconClass} />;
const IconWebIm: Component = () => <MessagesSquare class={iconClass} />;
const IconMatter: Component = () => <Radio class={iconClass} />;

export type LeafNode = {
  kind: 'leaf';
  id: TabId;
  labelKey:
    | 'navStatus'
    | 'navBasic'
    | 'navLlm'
    | 'navAsr'
    | 'navIm'
    | 'navWebReq'
    | 'navMemory'
    | 'navCapabilities'
    | 'navLuaModules'
    | 'navFiles'
    | 'navWebIm'
    | 'navMatter';
  icon: Component;
};
export type GroupNode = {
  kind: 'group';
  id: string;
  labelKey: 'navSystemSettings';
  icon: Component;
  children: LeafNode[];
};
export type NavNode = LeafNode | GroupNode;

export const NAV_TREE: NavNode[] = [
  { kind: 'leaf', id: 'status', labelKey: 'navStatus', icon: IconStatus },
  {
    kind: 'group',
    id: 'basic-settings',
    labelKey: 'navSystemSettings',
    icon: IconGear,
    children: [
      { kind: 'leaf', id: 'basic', labelKey: 'navBasic', icon: IconWifi },
      { kind: 'leaf', id: 'llm', labelKey: 'navLlm', icon: IconLlm },
      { kind: 'leaf', id: 'asr', labelKey: 'navAsr', icon: IconAsr },
      { kind: 'leaf', id: 'im', labelKey: 'navIm', icon: IconIm },
      { kind: 'leaf', id: 'webreq', labelKey: 'navWebReq', icon: IconSearch },
    ],
  },
  { kind: 'leaf', id: 'matter', labelKey: 'navMatter', icon: IconMatter },
  { kind: 'leaf', id: 'memory', labelKey: 'navMemory', icon: IconMemory },
  { kind: 'leaf', id: 'webim', labelKey: 'navWebIm', icon: IconWebIm },
  { kind: 'leaf', id: 'capabilities', labelKey: 'navCapabilities', icon: IconCaps },
  { kind: 'leaf', id: 'skills', labelKey: 'navLuaModules', icon: IconSkills },
  { kind: 'leaf', id: 'files', labelKey: 'navFiles', icon: IconFiles },
];

export const LEAF_IDS = collectLeafIds(NAV_TREE);

/** Leaf nodes hidden until a runtime capability flag turns them on. */
function isNodeVisible(node: NavNode): boolean {
  if (node.kind === 'leaf' && node.id === 'matter') {
    return appStatus()?.matter_enabled === true;
  }
  return true;
}

/** A group is shown only when at least one of its children is visible. */
function isGroupVisible(node: NavNode): boolean {
  if (node.kind !== 'group') return isNodeVisible(node);
  return node.children.some(isNodeVisible);
}

function collectLeafIds(nodes: NavNode[]): TabId[] {
  const out: TabId[] = [];
  for (const node of nodes) {
    if (node.kind === 'leaf') out.push(node.id);
    else out.push(...collectLeafIds(node.children));
  }
  return out;
}

function groupContains(group: GroupNode, id: TabId): boolean {
  return group.children.some((child) => child.id === id);
}

function groupDirty(group: GroupNode): boolean {
  return group.children.some((child) => isDirty(child.id));
}

type SidebarProps = {
  current: TabId;
  onSelect: (id: TabId) => void;
  collapsed: boolean;
  onToggleCollapsed: () => void;
  mobileOpen?: boolean;
  onCloseMobile?: () => void;
};

const EXPANDED_STORAGE_KEY = 'esp-claw-sidebar-expanded';

function readExpanded(): Set<string> {
  try {
    const raw = localStorage.getItem(EXPANDED_STORAGE_KEY);
    if (raw) {
      const parsed = JSON.parse(raw);
      if (Array.isArray(parsed)) {
        return new Set(parsed.filter((v) => typeof v === 'string'));
      }
    }
  } catch {
    /* ignore */
  }
  return new Set(['basic-settings']);
}

export const Sidebar: Component<SidebarProps> = (props) => {
  const [expanded, setExpanded] = createSignal<Set<string>>(readExpanded());
  const isMobileDrawer = () => props.mobileOpen !== undefined;
  const isCollapsed = () => !isMobileDrawer() && props.collapsed;

  const persistExpanded = (next: Set<string>) => {
    setExpanded(next);
    try {
      localStorage.setItem(EXPANDED_STORAGE_KEY, JSON.stringify(Array.from(next)));
    } catch {
      /* ignore */
    }
  };

  const toggleGroup = (id: string) => {
    const next = new Set(expanded());
    if (next.has(id)) next.delete(id);
    else next.add(id);
    persistExpanded(next);
  };

  const autoExpanded = createMemo(() => {
    const current = props.current;
    for (const node of NAV_TREE) {
      if (node.kind === 'group' && groupContains(node, current)) {
        if (!expanded().has(node.id)) {
          const next = new Set(expanded());
          next.add(node.id);
          persistExpanded(next);
        }
        return;
      }
    }
  });

  autoExpanded();

  const handleSelect = (id: TabId) => {
    props.onSelect(id);
    props.onCloseMobile?.();
  };

  const renderLeaf = (leaf: LeafNode, options: { indent?: boolean }) => (
    <button
      type="button"
      class={[
        'relative w-full flex items-center gap-3 rounded-[var(--radius-sm)] transition text-left',
        isCollapsed()
          ? 'justify-center py-2 px-0'
          : options.indent
            ? 'px-3 py-2 pl-8'
            : 'px-3 py-2',
        props.current === leaf.id
          ? 'bg-[var(--color-accent)]/15 text-[var(--color-text-primary)] ring-1 ring-[var(--color-accent-soft)]/40'
          : 'text-[var(--color-text-secondary)] hover:bg-white/5 hover:text-[var(--color-text-primary)]',
      ].join(' ')}
      onClick={() => handleSelect(leaf.id)}
      title={t(leaf.labelKey) as string}
    >
      <span class="relative inline-flex">
        <leaf.icon />
        <Show when={isCollapsed() && isDirty(leaf.id)}>
          <span
            class="absolute -top-1 -right-1 w-1.5 h-1.5 rounded-full bg-[var(--color-orange)]"
            title={t('unsavedIndicator') as string}
          />
        </Show>
      </span>
      <Show when={!isCollapsed()}>
        <span class="flex-1 text-[0.9rem] truncate">{t(leaf.labelKey)}</span>
        <Show when={isDirty(leaf.id)}>
          <span
            class="w-2 h-2 rounded-full bg-[var(--color-orange)]"
            title={t('unsavedIndicator') as string}
          />
        </Show>
      </Show>
    </button>
  );

  const renderGroup = (group: GroupNode) => {
    const isOpen = () => expanded().has(group.id) || isCollapsed();
    const activeChild = () => groupContains(group, props.current);
    return (
      <div>
        <button
          type="button"
          class={[
            'relative w-full flex items-center gap-3 rounded-[var(--radius-sm)] transition text-left',
            isCollapsed() ? 'justify-center py-2 px-0' : 'px-3 py-2',
            activeChild()
              ? 'text-[var(--color-text-primary)] bg-white/[0.04]'
              : 'text-[var(--color-text-secondary)] hover:bg-white/5 hover:text-[var(--color-text-primary)]',
          ].join(' ')}
          onClick={() => {
            if (isCollapsed()) {
              const first = group.children[0];
              if (first) handleSelect(first.id);
            } else {
              toggleGroup(group.id);
            }
          }}
          title={t(group.labelKey) as string}
          aria-expanded={expanded().has(group.id)}
        >
          <span class="relative inline-flex">
            <group.icon />
            <Show when={isCollapsed() && groupDirty(group)}>
              <span class="absolute -top-1 -right-1 w-1.5 h-1.5 rounded-full bg-[var(--color-orange)]" />
            </Show>
          </span>
          <Show when={!isCollapsed()}>
            <span class="flex-1 text-[0.9rem] truncate">{t(group.labelKey)}</span>
            <Show when={groupDirty(group)}>
              <span class="w-2 h-2 rounded-full bg-[var(--color-orange)]" />
            </Show>
            <Show when={!isMobileDrawer()}>
              <svg
                class={[
                  'w-3.5 h-3.5 text-[var(--color-text-muted)] transition-transform',
                  expanded().has(group.id) ? 'rotate-90' : '',
                ].join(' ')}
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="2"
                stroke-linecap="round"
                stroke-linejoin="round"
              >
                <path d="M9 6l6 6-6 6" />
              </svg>
            </Show>
          </Show>
        </button>
        <Show when={!isCollapsed() && isOpen()}>
          <ul class="flex flex-col gap-0.5 mt-0.5">
            <For each={group.children.filter(isNodeVisible)}>
              {(leaf) => <li>{renderLeaf(leaf, { indent: true })}</li>}
            </For>
          </ul>
        </Show>
      </div>
    );
  };

  return (
    <aside
      class={[
        isMobileDrawer()
          ? [
              'fixed inset-y-0 left-0 z-50 w-72 max-w-[88vw] border-r border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)]/95 backdrop-blur-md flex flex-col transition-transform duration-200 lg:hidden',
              props.mobileOpen ? 'translate-x-0' : '-translate-x-full',
            ].join(' ')
          : [
              'hidden lg:flex border-r border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)]/60 flex-col transition-[width] duration-200',
              props.collapsed ? 'w-16' : 'w-60',
            ].join(' '),
      ].join(' ')}
    >
      <div
        class={[
          'flex items-center h-14 border-b border-[var(--color-border-subtle)]',
          !isMobileDrawer() && props.collapsed ? 'justify-center px-2' : 'justify-between px-3',
        ].join(' ')}
      >
        <Show
          when={!isMobileDrawer() && props.collapsed}
          fallback={
            <>
              <div class="min-w-0 flex-1 ml-1 pr-3 flex items-center gap-2">
                <Show
                  when={isMobileDrawer()}
                  fallback={<Logo class="min-w-0" />}
                >
                  <ClawGlyph class="w-6 h-6 shrink-0" />
                  <StatusSummary compact class="text-[0.74rem]" />
                </Show>
              </div>
              <button
                type="button"
                onClick={isMobileDrawer() ? props.onCloseMobile : props.onToggleCollapsed}
                class="inline-flex items-center justify-center w-9 h-9 rounded-[var(--radius-sm)] text-[var(--color-text-muted)] hover:bg-white/5 hover:text-[var(--color-text-primary)] transition shrink-0"
                aria-label={
                  isMobileDrawer() ? (t('closeMenu') as string) : (t('toggleSidebar') as string)
                }
              >
                <Show
                  when={isMobileDrawer()}
                  fallback={
                    <svg
                      class="w-4 h-4"
                      viewBox="0 0 24 24"
                      fill="none"
                      stroke="currentColor"
                      stroke-width="2"
                      stroke-linecap="round"
                      stroke-linejoin="round"
                    >
                      <path d="M15 18l-6-6 6-6" />
                    </svg>
                  }
                >
                  <svg
                    class="w-4 h-4"
                    viewBox="0 0 24 24"
                    fill="none"
                    stroke="currentColor"
                    stroke-width="2"
                    stroke-linecap="round"
                    stroke-linejoin="round"
                  >
                    <path d="M18 6L6 18" />
                    <path d="M6 6l12 12" />
                  </svg>
                </Show>
              </button>
            </>
          }
        >
          <button
            type="button"
            onClick={props.onToggleCollapsed}
            class="inline-flex items-center justify-center w-10 h-10 rounded-[var(--radius-sm)] hover:bg-white/5 transition"
            aria-label={t('toggleSidebar') as string}
          >
            <ClawGlyph class="w-6 h-6" />
          </button>
        </Show>
      </div>
      <nav class="flex-1 overflow-y-auto py-2">
        <ul class="flex flex-col gap-0.5 px-2">
          <For each={NAV_TREE.filter(isGroupVisible)}>
            {(node) => (
              <li>
                {node.kind === 'leaf' ? renderLeaf(node, { indent: false }) : renderGroup(node)}
              </li>
            )}
          </For>
        </ul>
      </nav>
    </aside>
  );
};
