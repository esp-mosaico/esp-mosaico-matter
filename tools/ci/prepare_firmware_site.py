#!/usr/bin/env python3
"""Package merged firmware and the download page for GitHub Pages."""

import html
import os
import shutil
from pathlib import Path

VARIANTS = ('matter_hub_claw', 'matter_hub_xiaozhi', 'matter_hub_no_agent', 'matter_bridge')


def main():
    repo = Path(__file__).resolve().parents[2]
    images = repo / 'build/firmware/images'
    artifacts = []
    revision = None
    for variant in VARIANTS:
        image = images / f'{variant}.bin'
        record = image.with_suffix('.txt')
        for path in (image, record):
            if not path.is_file() or not path.stat().st_size:
                raise SystemExit(f'Missing firmware artifact: {path}')
        current = record.read_text()
        if revision is not None and current != revision:
            raise SystemExit(f'Firmware build commits differ: {variant}')
        revision = current
        artifacts.extend((image, record))
    revisions = dict(line.split(': ', 1) for line in revision.splitlines() if ': ' in line)
    repositories = {
        'esp-mosaico-matter': os.environ.get('GITHUB_REPOSITORY', 'esp-mosaico/esp-mosaico-matter'),
        'esp-idf': 'espressif/esp-idf',
    }
    commits = []
    for project, repository in repositories.items():
        if not revisions.get(project):
            raise SystemExit(f'Missing firmware revision: {project}')
        commit = revisions[project]
        url = f'https://github.com/{repository}/commit/{commit}'
        commits.append(f'<li>{project}: <a href="{html.escape(url, quote=True)}">{html.escape(commit[:8])}</a></li>')
    source_url = f'https://github.com/{repositories["esp-mosaico-matter"]}/tree/{revisions["esp-mosaico-matter"]}'
    page = Path(__file__).with_name('firmware.html').read_text()
    page = page.replace('@SOURCE_URL@', html.escape(source_url, quote=True))
    page = page.replace('@FIRMWARE_COMMITS@', '\n'.join(commits))
    destination = repo / 'build/firmware/site'
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    (destination / 'index.html').write_text(page)
    for path in artifacts:
        shutil.copy2(path, destination / path.name)


if __name__ == '__main__':
    main()
