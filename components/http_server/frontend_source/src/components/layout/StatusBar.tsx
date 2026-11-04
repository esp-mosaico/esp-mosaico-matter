import { Show, type Component, type JSX } from 'solid-js';
import { t } from '../../i18n';
import { appStatus } from '../../state/config';

/** Mosaico lettering from the product's existing ESP-Mosaico wordmark. */
export const Logo: Component<{ class?: string }> = (props) => (
  <svg
    xmlns="http://www.w3.org/2000/svg"
    viewBox="978 62 456 73"
    class={['h-5 w-auto shrink-0 select-none', props.class ?? ''].join(' ').trim()}
    role="img"
    aria-label="Mosaico"
    fill="#ff4c01"
  >
    <path d="M1048.795 131.925v-67.2h-9.408l-24.48 48.384-24.576-48.384h-9.504v67.2h7.296V76.533l24.384 48.288h4.608l24.48-48.288.096 55.392zm49.344-67.392c-19.68 0-35.232 14.784-35.232 33.792 0 18.912 15.552 33.984 35.232 33.984 19.584 0 35.136-15.072 35.136-33.984s-15.552-33.792-35.136-33.792m0 7.296c14.976 0 27.264 11.712 27.264 26.496s-12.288 26.592-27.264 26.592-27.456-11.808-27.456-26.592 12.48-26.496 27.456-26.496m69.12-7.392c-13.92 0-23.232 6.816-23.232 17.376 0 24.096 37.824 14.688 37.728 32.352 0 6.432-6.048 10.368-15.744 10.368-7.584 0-16.128-3.456-22.368-9.504l-3.456 6.912c6.336 6.048 15.936 10.272 25.728 10.272 14.4 0 24.288-7.2 24.288-18.432.096-24.384-37.728-15.36-37.728-32.64 0-5.664 5.664-8.928 14.304-8.928 5.376 0 12.48 1.632 18.528 5.664l3.264-7.2c-5.568-3.744-13.536-6.24-21.312-6.24m80.448 50.496 7.488 16.992h8.352l-30.048-67.2h-7.968l-30.144 67.2h8.16l7.488-16.992zm-3.264-7.392h-30.144l14.976-34.08zm37.344 24.384v-67.2h-7.68v67.2zm49.056-67.392c-19.488 0-35.04 14.88-35.04 33.696 0 19.008 15.36 34.08 34.752 34.08 9.312 0 18.336-4.128 24.96-10.368l-4.704-5.28c-5.28 4.992-12.48 8.064-19.872 8.064-15.168 0-27.36-11.808-27.36-26.496 0-14.784 12.192-26.496 27.36-26.496 7.392 0 14.688 3.168 19.872 8.448l4.608-5.856c-6.432-6.048-15.36-9.792-24.576-9.792m65.76 0c-19.68 0-35.232 14.784-35.232 33.792 0 18.912 15.552 33.984 35.232 33.984 19.584 0 35.136-15.072 35.136-33.984s-15.552-33.792-35.136-33.792m0 7.296c14.976 0 27.264 11.712 27.264 26.496s-12.288 26.592-27.264 26.592-27.456-11.808-27.456-26.592 12.48-26.496 27.456-26.496" />
  </svg>
);

/** Share the official Espressif symbol with the browser's tab icon. */
export const EspressifMark: Component<{ class?: string }> = (props) => (
  <img
    src="/favicon.svg"
    width="24"
    height="24"
    class={props.class}
    alt=""
    aria-hidden="true"
  />
);

export const StatusSummary: Component<{ class?: string; compact?: boolean }> = (props) => {
  const online = () => appStatus()?.wifi_connected === true;
  const loading = () => appStatus() === null;

  return (
    <div
      class={[
        'min-w-0 flex items-center gap-2 text-[0.8rem]',
        props.compact ? 'flex-nowrap overflow-hidden' : 'flex-wrap',
        props.class ?? '',
      ]
        .join(' ')
        .trim()}
    >
      <span
        class={[
          'inline-flex items-center gap-2 px-3 py-1 rounded-full border font-medium min-w-0',
          loading()
            ? 'border-[var(--color-border-subtle)] text-[var(--color-text-muted)] bg-white/[0.04]'
            : online()
              ? 'border-[rgba(104,211,145,0.2)] bg-[var(--color-green-dim)] text-[var(--color-green)]'
              : 'border-[var(--color-border-subtle)] bg-white/[0.04] text-[var(--color-text-muted)]',
        ].join(' ')}
      >
        <span
          class={[
            'w-1.5 h-1.5 rounded-full',
            online() ? 'bg-[var(--color-green)] pulse-dot' : 'bg-[var(--color-text-muted)]',
          ].join(' ')}
        />
        <span class={props.compact ? 'truncate' : ''}>
          {loading()
            ? t('statusLoading')
            : online()
              ? t('statusOnline')
              : appStatus()?.ap_active
                ? t('statusApActive')
                : t('statusOffline')}
        </span>
      </span>
      <Show when={appStatus()?.ip}>
        <span
          class={[
            'text-[var(--color-border-subtle)] select-none',
            props.compact ? 'shrink-0' : '',
          ].join(' ')}
        >
          ·
        </span>
        <span
          class={[
            'font-mono text-[0.78rem] text-[var(--color-text-secondary)]',
            props.compact ? 'truncate min-w-0' : '',
          ].join(' ')}
        >
          IP: {appStatus()!.ip}
        </span>
      </Show>
      <Show when={appStatus()?.storage_base_path}>
        <span
          class={[
            'text-[var(--color-border-subtle)] select-none',
            props.compact ? 'shrink-0' : '',
          ].join(' ')}
        >
          ·
        </span>
        <span
          class={[
            'font-mono text-[0.78rem] text-[var(--color-text-secondary)]',
            props.compact ? 'truncate min-w-0' : '',
          ].join(' ')}
        >
          Storage: {appStatus()!.storage_base_path}
        </span>
      </Show>
    </div>
  );
};

type StatusBarProps = {
  leadingSlot?: JSX.Element;
  slot?: () => any;
};

export const StatusBar: Component<StatusBarProps> = (props) => {
  return (
    <header class="flex items-center justify-between gap-3 h-14 px-4 sm:px-5 border-b border-[var(--color-border-subtle)] bg-[rgba(10,11,14,0.85)] backdrop-blur-md sticky top-0 z-40">
      <div class="flex items-center gap-3 sm:gap-4 min-w-0">
        {props.leadingSlot}
        <div class="hidden lg:flex">
          <StatusSummary />
        </div>
      </div>
      <div class="flex items-center gap-2">{props.slot?.()}</div>
    </header>
  );
};
