import { Show, type Component, type JSX } from 'solid-js';
import { t } from '../../i18n';
import { appStatus } from '../../state/config';

/**
 * ESP-Claw horizontal word mark, vendored from
 * esp-mosaico-claw/third-party/esp-claw/docs/src/assets/logos/logo.svg
 * ("ESP" follows the theme via currentColor, "CLAW" + claw glyph stay brand red).
 */
export const EspClawWordmark: Component<{ class?: string }> = (props) => (
  <svg
    xmlns="http://www.w3.org/2000/svg"
    viewBox="0 0 686.46 191.26"
    class={props.class ?? 'h-5 w-auto'}
    aria-hidden="true"
  >
    <path
      fill="currentColor"
      d="M274.61 59.98v13.2h-37.7v15.3h34.6v12.2h-34.6v17.5h38.5v13.2h-54.2v-71.4zm23.4 53.8q1.305 2.505 3.45 4.05c1.43 1.03 3.12 1.8 5.05 2.3q2.895.75 6 .75c1.4 0 2.9-.12 4.5-.35q2.4-.345 4.5-1.35c1.4-.67 2.57-1.58 3.5-2.75s1.4-2.65 1.4-4.45c0-1.93-.62-3.5-1.85-4.7s-2.85-2.2-4.85-3-4.27-1.5-6.8-2.1-5.1-1.27-7.7-2c-2.67-.67-5.27-1.48-7.8-2.45s-4.8-2.22-6.8-3.75-3.62-3.45-4.85-5.75-1.85-5.08-1.85-8.35c0-3.67.78-6.85 2.35-9.55q2.355-4.05 6.15-6.75c2.53-1.8 5.4-3.13 8.6-4s6.4-1.3 9.6-1.3c3.73 0 7.32.42 10.75 1.25q5.145 1.245 9.15 4.05c2.67 1.87 4.78 4.25 6.35 7.15s2.35 6.42 2.35 10.55h-15.2c-.13-2.13-.58-3.9-1.35-5.3s-1.78-2.5-3.05-3.3-2.72-1.37-4.35-1.7-3.42-.5-5.35-.5c-1.27 0-2.53.13-3.8.4s-2.42.73-3.45 1.4-1.88 1.5-2.55 2.5-1 2.27-1 3.8c0 1.4.27 2.53.8 3.4q.795 1.305 3.15 2.4c1.57.73 3.73 1.47 6.5 2.2s6.38 1.67 10.85 2.8c1.33.27 3.18.75 5.55 1.45s4.72 1.82 7.05 3.35 4.35 3.58 6.05 6.15 2.55 5.85 2.55 9.85c0 3.27-.63 6.3-1.9 9.1s-3.15 5.22-5.65 7.25c-2.5 2.04-5.6 3.62-9.3 4.75s-7.98 1.7-12.85 1.7c-3.93 0-7.75-.48-11.45-1.45-3.7-.96-6.97-2.48-9.8-4.55s-5.08-4.7-6.75-7.9q-2.505-4.8-2.4-11.4h15.2c0 2.4.43 4.43 1.3 6.1m85.1-53.8q6.705 0 11.4 1.95c3.13 1.3 5.68 3.02 7.65 5.15 1.96 2.13 3.4 4.57 4.3 7.3s1.35 5.57 1.35 8.5-.45 5.68-1.35 8.45-2.33 5.22-4.3 7.35-4.52 3.85-7.65 5.15q-4.695 1.95-11.4 1.95h-16.5v25.6h-15.7v-71.4zm-4.3 33.6c1.8 0 3.53-.13 5.2-.4s3.13-.78 4.4-1.55 2.28-1.85 3.05-3.25 1.15-3.23 1.15-5.5-.38-4.1-1.15-5.5-1.78-2.48-3.05-3.25-2.73-1.28-4.4-1.55-3.4-.4-5.2-.4h-12.2v21.4zm36.06 10.09h24.74v9.18h-24.74z"
    />
    <path
      fill="#e8362d"
      d="M487.08 78.23c-3.09-2.2-6.76-3.29-11.01-3.29-3.63 0-6.73.69-9.31 2.08s-4.7 3.25-6.38 5.59c-1.67 2.34-2.9 5.01-3.7 8.02a36.7 36.7 0 0 0-1.19 9.36c0 3.53.4 6.86 1.19 10.01s2.03 5.9 3.7 8.24 3.81 4.21 6.42 5.59c2.61 1.39 5.72 2.08 9.35 2.08 2.66 0 5.03-.45 7.1-1.34s3.85-2.14 5.36-3.73c1.5-1.59 2.68-3.48 3.53-5.68q1.275-3.3 1.53-7.11h8.08c-.79 7.8-3.43 13.87-7.9 18.21-4.48 4.33-10.6 6.5-18.36 6.5-4.7 0-8.81-.82-12.33-2.47-3.51-1.65-6.43-3.92-8.75-6.81s-4.07-6.3-5.23-10.23q-1.74-5.895-1.74-12.66c0-6.765.62-8.74 1.87-12.7s3.07-7.41 5.48-10.36 5.41-5.27 9.01-6.98c3.6-1.7 7.72-2.56 12.37-2.56 3.17 0 6.18.43 9.01 1.3s5.36 2.14 7.57 3.81c2.21 1.68 4.05 3.77 5.53 6.29 1.47 2.51 2.44 5.42 2.89 8.71h-8.08c-.91-4.39-2.9-7.69-5.99-9.88zm33.87-8.76v54.97h32.13v6.94h-40.21v-61.9h8.08zm65.71 0 23.71 61.9h-8.92l-6.63-18.64h-25.67l-6.8 18.64h-8.25l23.63-61.9h8.92zm5.61 36.33-10.12-28.87h-.17l-10.29 28.87h20.57zm69.53 25.58-13.86-52.02h-.17l-14.02 52.02h-8.42l-15.56-61.9h8.25l11.9 51.5h.17l13.6-51.5h8.76l13.43 51.5h.17l12.33-51.5h8.08l-16.24 61.9zM165.38 113v.06l-.04.54v.04c-1.29 20.81-10.26 39.52-24.09 53.36-15 15-35.69 24.26-58.57 24.26-9.69 0-18.97-1.66-27.6-4.71C24.43 175.72 2.07 147.36.03 113.53c-.01-.15-.03-.32-.03-.47 0-1.27.52-2.42 1.36-3.26s1.98-1.36 3.26-1.36h156.15c1.29 0 2.43.52 3.27 1.36.82.82 1.34 1.96 1.36 3.2zM55.07 37.15v52.87c0 1.27-.52 2.42-1.36 3.26-.82.84-1.98 1.36-3.26 1.36H6.65c-2.54 0-4.61-2.07-4.61-4.61 0-.31.03-.6.08-.88l.13-.49a82.4 82.4 0 0 1 21.86-38.77c7.03-7.03 15.3-12.8 24.47-16.95.03-.01.04-.01.07-.03.54-.24 1.16-.36 1.8-.36 1.27 0 2.43.52 3.26 1.36.84.82 1.36 1.98 1.36 3.26zm108.26 52.87c0 1.27-.52 2.42-1.36 3.26s-2 1.36-3.27 1.36H87.28c-2.54 0-4.61-2.07-4.61-4.61V4.61c0-1.27.52-2.43 1.36-3.27a4.645 4.645 0 0 1 5.06-.98c.14.06.27.13.41.2 14.45 6.65 27.52 15.83 38.64 26.96 15.44 15.44 27.14 34.65 33.58 56.13.03.06.04.13.06.18.46 1.5.88 3.01 1.27 4.53.03.1.06.2.07.31.01.03.01.06.03.08.03.13.07.25.08.38.07.29.1.6.1.91z"
    />
  </svg>
);

/** Official "matter" word mark (Wikimedia: Logo_of_Matter_connectivity_standard.svg). */
export const MatterWordmark: Component<{ class?: string }> = (props) => (
  <svg
    xmlns="http://www.w3.org/2000/svg"
    viewBox="0 0 338.667 72.644"
    class={props.class ?? 'h-[1.1rem] w-auto'}
    fill="currentColor"
    aria-hidden="true"
  >
    <path d="M294.697 60.122c6.777 0 12.314-3.055 15.781-7.762l-3.725-2.121c-2.691 3.415-6.831 5.641-12.056 5.641-8.464 0-14.985-5.832-15.666-13.294h33.565c.024-.441.05-.881.05-1.293 0-10.397-8.482-18.831-18.854-18.831s-18.802 8.435-18.802 18.832 8.43 18.829 19.706 18.829zm-.877-33.419c7.068 0 13.213 5.068 14.568 11.796h-29.139c1.346-6.727 7.455-11.796 14.571-11.796zm-99.896-4.243c-10.398 0-18.83 8.435-18.83 18.833s8.432 18.83 18.83 18.83c6.281 0 11.468-3.078 14.59-7.806v7.082h4.24V23.184h-4.24v7.082c-3.122-4.728-8.309-7.806-14.59-7.806zm0 4.243c8.07 0 14.59 6.518 14.59 14.59s-6.52 14.587-14.59 14.587a14.57 14.57 0 0 1-14.587-14.587c0-8.072 6.517-14.59 14.587-14.59zM65.715 32.905c-7.996 2.19-15.164 7.406-19.636 15.152s-5.407 16.568-3.306 24.587l7.835-4.526a23.9 23.9 0 0 1 1.105-11.836l18.309 10.569 4.303-2.487v-4.967L56.016 48.829a23.92 23.92 0 0 1 9.699-6.879zm-57.108 0v9.045a23.91 23.91 0 0 1 9.699 6.879L0 59.398v4.967l4.303 2.487 18.306-10.569c1.39 3.868 1.726 7.938 1.108 11.836l7.832 4.526c2.101-8.02 1.167-16.841-3.306-24.587A32.52 32.52 0 0 0 8.607 32.905zM337.063 22.46c-8.542 0-15.466 6.448-15.466 15.47v21.469h4.243V37.93c0-6.68 5.025-11.227 11.223-11.227h1.604V22.46zm-213.131 0c-8.542 0-15.466 6.448-15.466 15.47v21.469h4.243V37.93c0-6.68 5.023-11.227 11.223-11.227s11.227 4.547 11.227 11.227v21.469h4.243V37.93c0-6.68 5.023-11.227 11.223-11.227s11.227 4.547 11.227 11.227v21.469h4.243V37.93c0-9.021-6.927-15.47-15.47-15.47-5.535 0-10.576 2.848-13.37 8.642-2.845-5.741-7.84-8.642-13.323-8.642zm108.531-11.636l-4.24 2.43v9.931h-5.691v4.087h5.691v23.847c0 4.605 3.621 8.279 8.225 8.279h6.307v-4.243h-6.307c-2.175 0-3.986-1.811-3.986-4.087V27.271h21.574v23.847c0 4.605 3.619 8.279 8.171 8.279h6.364v-4.243h-6.364c-2.12 0-3.929-1.811-3.929-4.087V27.271h10.293v-4.087H258.28v-12.36l-4.243 2.43v9.931h-21.574zM37.161 0l-4.303 2.484v21.138c-4.046-.731-7.736-2.476-10.804-4.961l-7.838 4.522c5.895 5.83 14 9.429 22.946 9.429s17.051-3.599 22.946-9.429l-7.835-4.522a23.92 23.92 0 0 1-10.807 4.961V2.484z" />
  </svg>
);

/**
 * Header lockup: the ESP-Claw word mark joined to the official Matter word mark.
 * Replaces the upstream ESP-MOSAICO logo.
 */
export const Logo: Component<{ class?: string }> = (props) => (
  <span
    class={['inline-flex items-center gap-2.5 select-none', props.class ?? ''].join(' ').trim()}
    aria-label="ESP-Claw Matter"
  >
    <EspClawWordmark class="h-[1.35rem] w-auto text-[var(--color-text-primary)]" />
    <span class="h-4 w-px bg-[var(--color-border-subtle)]" aria-hidden="true" />
    <MatterWordmark class="h-[1rem] w-auto text-[var(--color-text-secondary)]" />
  </span>
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
