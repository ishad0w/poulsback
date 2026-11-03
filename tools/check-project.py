#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Check the public contract and documentation without touching hardware."""

import argparse
from fnmatch import fnmatchcase
import hashlib
from html.parser import HTMLParser
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys
import unicodedata
from urllib.parse import unquote, urlsplit


def require(condition, message):
    if not condition:
        raise ValueError(message)


def parse(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'duplicate JSON key')
            result[key] = value
        return result

    def constant(value):
        raise ValueError('nonfinite JSON number')

    value = json.loads(raw, object_pairs_hook=pairs, parse_constant=constant)
    json.dumps(value, allow_nan=False)
    return value


def fields(value, expected, label):
    require(type(value) is dict and set(value) == set(expected), label + ' fields')


def fingerprint(value):
    raw = json.dumps(
        value, sort_keys=True, separators=(',', ':'), ensure_ascii=False, allow_nan=False
    ).encode()
    return hashlib.sha256(raw).hexdigest()


BACKENDS = {'SOURCE', 'CPU_VM', 'HARDWARE', 'FEASIBILITY'}


def plain_line(value, label):
    require(
        type(value) is str
        and value.strip()
        and all(unicodedata.category(c) not in ('Cc', 'Cf', 'Zl', 'Zp') for c in value)
        and not any(c in value for c in '<>`'),
        label + ' must be plain single-line text',
    )


def identifiers(values, label):
    require(
        type(values) is list
        and all(type(x) is str and re.fullmatch(r'[a-z0-9]+(?:[.-][a-z0-9]+)*', x) for x in values)
        and len(values) == len(set(values)),
        label + ' IDs',
    )


def graph(nodes, label):
    identifiers([n['id'] for n in nodes], label)
    for node in nodes:
        identifiers(node['prerequisites'], label + ' prerequisites')
    edges = {n['id']: set(n['prerequisites']) for n in nodes}
    require(all(e <= edges.keys() for e in edges.values()), label + ' unknown prerequisite')
    while edges:
        ready = {key for key, deps in edges.items() if not deps}
        require(ready, label + ' dependency cycle')
        edges = {key: deps - ready for key, deps in edges.items() if key not in ready}


def ordinary(root, relative):
    current = root
    for part in PurePosixPath(relative).parts:
        current /= part
        require(not current.is_symlink(), 'symlink in project input')
    require(current.is_file(), 'missing ordinary project file: ' + relative)
    return current


def scan_text(relative, text):
    # Reject developer-specific paths and credential material in text inputs.
    require(
        not re.search(r'/(?:home|Users)/[A-Za-z0-9_.-]+/', text),
        'developer absolute path in ' + relative,
    )
    require(
        not re.search(r'-----BEGIN (?:OPENSSH |RSA |EC |DSA )?PRIVATE KEY-----', text),
        'private key in ' + relative,
    )
    require(
        not re.search(r'\b(?:gh[pousr]_[A-Za-z0-9]{25,}|github_pat_[A-Za-z0-9_]{25,})\b', text),
        'credential token in ' + relative,
    )
    require(
        not re.search(r'(?im)^\s*(?:password|passwd|secret|token)\s*[:=]\s*["\']?[^\s"\']+', text),
        'literal credential assignment in ' + relative,
    )


def markdown_prose(text):
    """Exclude fenced examples and HTML comments from document checks."""
    result, fence = [], None
    for line in text.splitlines():
        marker = re.match(r'^ {0,3}(`{3,}|~{3,})', line)
        if marker:
            value = marker[1]
            if fence is None:
                fence = value
            elif value[0] == fence[0] and len(value) >= len(fence):
                fence = None
            continue
        if fence is None:
            result.append(line)
    return re.sub(r'<!--.*?-->', '', '\n'.join(result), flags=re.S)


def markdown_anchors(text):
    """GitHub-style ATX heading anchors, including duplicate suffixes."""
    prose = markdown_prose(text)
    html = re.sub(r'(`+).*?\1(?!`)', '', prose, flags=re.S)
    explicit = []

    class AnchorParser(HTMLParser):
        def handle_starttag(self, tag, attrs):
            if tag != 'a':
                return
            names = [name for name, _ in attrs if name in ('id', 'name')]
            require(len(names) == len(set(names)), 'duplicate explicit anchor attribute')
            explicit.extend(value for name, value in attrs if name in names and value)

    AnchorParser().feed(html)
    require(len(explicit) == len(set(explicit)), 'duplicate explicit Markdown anchor')
    anchors = set(explicit)
    for heading in re.findall(r'^ {0,3}#{1,6}\s+(.+?)\s*#*\s*$', prose, re.M):
        heading = re.sub(r'\[([^\]]+)\]\([^)]*\)', r'\1', heading)
        heading = re.sub(r'<[^>]+>', '', heading).lower()
        slug = ''.join(
            c for c in heading if c in ' -_' or unicodedata.category(c)[0] in 'LN'
        ).replace(' ', '-')
        anchor, suffix = slug, 0
        while anchor in anchors:
            suffix += 1
            anchor = slug + '-' + str(suffix)
        anchors.add(anchor)
    return anchors


def markdown_targets(text):
    prose = markdown_prose(text)
    targets = re.findall(r'!?\[[^\]]*\]\(([^\n]+?)\)', prose)

    class LinkParser(HTMLParser):
        def handle_starttag(self, tag, attrs):
            attribute = {'a': 'href', 'img': 'src'}.get(tag)
            if attribute:
                targets.extend(value for name, value in attrs if name == attribute and value)

    LinkParser().feed(re.sub(r'(`+).*?\1(?!`)', '', prose, flags=re.S))
    for target in targets:
        target = target.strip()
        if target.startswith('<') and target.endswith('>'):
            target = target[1:-1]
        target = target.split(' "', 1)[0]
        yield target, urlsplit(target)


def markdown_links(root, relative, text):
    for target, parsed in markdown_targets(text):
        if parsed.scheme or parsed.netloc:
            require(
                parsed.scheme in ('http', 'https', 'mailto'),
                'unsupported Markdown URL scheme in ' + relative,
            )
            continue
        value = unquote(parsed.path)
        require(not Path(value).is_absolute(), 'absolute local Markdown link in ' + relative)
        destination = (root / relative).parent / value if value else root / relative
        resolved = destination.resolve()
        require(resolved.is_relative_to(root.resolve()), 'escaping Markdown link in ' + relative)
        require(destination.exists(), 'broken Markdown link in ' + relative + ': ' + target)
        if parsed.fragment and destination.suffix.lower() == '.md':
            contents = text if not value else destination.read_text(encoding='utf-8')
            require(
                unquote(parsed.fragment) in markdown_anchors(contents),
                'broken Markdown anchor in ' + relative + ': ' + target,
            )


def documentation_routes(root):
    """Require readable routes to guides and a map entry for each docs owner."""
    root = root.resolve()

    def links(relative):
        text = ordinary(root, relative).read_text(encoding='utf-8')
        markdown_links(root, relative, text)
        result = set()
        for _, parsed in markdown_targets(text):
            if parsed.scheme or parsed.netloc:
                continue
            destination = ((root / relative).parent / unquote(parsed.path)).resolve()
            if parsed.path and destination.suffix == '.md':
                result.add(destination.relative_to(root).as_posix())
        return result

    guides = {p.relative_to(root).as_posix() for p in (root / 'docs').rglob('*.md')}
    visited, pending = set(), {'README.md'}
    while pending:
        relative = pending.pop()
        visited.add(relative)
        pending.update(links(relative) - visited)
    required = guides | {'AGENTS.md', 'MAP.md', 'MEMORY.md'}
    require(
        not required - visited,
        'documents unreachable from README: ' + ', '.join(sorted(required - visited)),
    )
    missing = guides - links('MAP.md')
    require(not missing, 'documents missing from MAP: ' + ', '.join(sorted(missing)))


def project_files(root):
    # Portable source selection also works in release archives without .git.
    # Root outputs mirror .gitignore; a source directory named build is valid.
    root_outputs = {'.git', 'build', 'out', '.local', '.ruff_cache', '.cache'}
    generated = (
        '*.pyc',
        '*.o',
        '*.a',
        '*.so',
        '*.ko',
        '*.mod.c',
        '*.cmd',
        'Module.symvers',
        'modules.order',
        'compile_commands.json',
    )

    def omitted(relative):
        return (
            '__pycache__' in relative.parts
            or relative.parts[0] in root_outputs
            or len(relative.parts) == 1
            and (relative.name == '.env' or relative.name.startswith('.env.'))
            or any(fnmatchcase(relative.name, pattern) for pattern in generated)
        )

    def traversal_error(error):
        raise error

    selected = []
    for directory, directories, filenames in os.walk(
        root, topdown=True, onerror=traversal_error, followlinks=False
    ):
        parent = Path(directory)
        directories[:] = sorted(
            name for name in directories if not omitted((parent / name).relative_to(root))
        )
        for name in directories:
            require(not (parent / name).is_symlink(), 'symlinked project content')
        for name in filenames:
            path = parent / name
            relative = path.relative_to(root)
            if omitted(relative):
                continue
            require(not path.is_symlink(), 'symlinked project content')
            if path.is_file():
                selected.append((path, relative.as_posix()))
    yield from sorted(selected)


def tool_documentation(root):
    """Every maintained tool, including imported helpers, needs a reader route."""
    root = root.resolve()
    guide = 'docs/tools.md'
    text = ordinary(root, guide).read_text(encoding='utf-8')
    markdown_links(root, guide, text)
    linked = set()
    for _, parsed in markdown_targets(text):
        if parsed.scheme or parsed.netloc or not parsed.path:
            continue
        destination = (root / guide).parent / unquote(parsed.path)
        linked.add(destination.resolve().relative_to(root).as_posix())
    tools = {p.relative_to(root).as_posix() for p in (root / 'tools').rglob('*.py')}
    missing = tools - linked
    require(not missing, 'tools missing from docs/tools.md: ' + ', '.join(sorted(missing)))


def contract_sha(catalog):
    """Identity of this catalog, not a hard-coded approval or run result."""
    return fingerprint(catalog)


def criteria_reference(value, expected_id=None):
    plain_line(value, 'criteria reference')
    require(type(value) is str and value.count('#') == 1, 'criteria reference')
    path, anchor = value.split('#')
    require(path.endswith('.md') and anchor, 'criteria Markdown anchor')
    require(
        '\\' not in path
        and ':' not in path
        and PurePosixPath(path).parts
        and not Path(path).is_absolute()
        and '..' not in PurePosixPath(path).parts
        and PurePosixPath(path).as_posix() == path,
        'criteria path',
    )
    require(not urlsplit(value).scheme, 'local criteria reference')
    if expected_id is not None:
        require(anchor == expected_id, 'criteria anchor must match its stable ID')


def project_path(value, label):
    require(
        type(value) is str
        and value
        and '\\' not in value
        and not Path(value).is_absolute()
        and '..' not in PurePosixPath(value).parts
        and PurePosixPath(value).as_posix() == value,
        label,
    )
    plain_line(value, label)


def validate_catalog_layout(catalog):
    fields(
        catalog,
        (
            'schema',
            'coverage_policy',
            'required_for_release',
            'domains',
            'integration_milestones',
            'retired',
        ),
        'catalog',
    )
    require(catalog['schema'] == 'poulsback-dod-catalog-v4', 'catalog schema')
    policy = catalog['coverage_policy']
    fields(
        policy,
        (
            'optional_domains',
            'assessment_domains',
            'criteria_ref',
            'm1_domains',
            'after_m1_domains',
            'exclusions_ref',
        ),
        'coverage policy',
    )
    criteria_reference(policy['criteria_ref'], 'acceptance-rules')
    project_path(policy['exclusions_ref'], 'exclusions reference')
    require(type(catalog['domains']) is list and catalog['domains'], 'domains')
    require(type(catalog['integration_milestones']) is list, 'integration milestones')
    counts = dict(domains=0, requirements=0, mandatory_requirements=0, case_families=0)
    all_requirements, all_cases = [], []

    def suite(node):
        require(
            type(node['required_backend']) is str
            and node['required_backend'] in BACKENDS | {'MIXED'},
            'required backend',
        )
        criteria_reference(node['criteria_ref'], node['id'])
        require(type(node['cases']) is list and node['cases'], 'nonempty case families')
        backends = set()
        for case in node['cases']:
            fields(
                case,
                (
                    'id',
                    'required_backend',
                    'parameters',
                    'artifacts',
                    'runner_suite',
                    'criteria_ref',
                    'case_list',
                ),
                'case',
            )
            identifiers([case['id']], 'case')
            require(case['id'].startswith(node['id'].split('.')[0] + '.'), 'case namespace')
            require(
                type(case['required_backend']) is str and case['required_backend'] in BACKENDS,
                'case backend',
            )
            backends.add(case['required_backend'])
            require(type(case['parameters']) is dict and case['parameters'], 'case parameters')
            parameter_json = json.dumps(case['parameters'], allow_nan=False)
            require(
                'coverage_policy.evidence_dispositions' not in parameter_json,
                'removed evidence disposition policy: use coverage_policy.criteria_ref',
            )
            artifacts = case['artifacts']
            require(
                type(artifacts) is list
                and artifacts
                and all(
                    type(x) is str
                    and x
                    and '\\' not in x
                    and PurePosixPath(x).parts
                    and not PurePosixPath(x).is_absolute()
                    and '..' not in PurePosixPath(x).parts
                    and PurePosixPath(x).as_posix() == x
                    for x in artifacts
                )
                and len(artifacts) == len(set(artifacts)),
                'artifact paths',
            )
            for artifact in artifacts:
                plain_line(artifact, 'artifact path')
            criteria_reference(case['criteria_ref'], case['id'])
            if case['case_list'] is not None:
                project_path(case['case_list'], 'case list')
                require(
                    case['case_list'] == 'spec/cases/' + case['id'] + '.json',
                    'case list path must be spec/cases/<case ID>.json',
                )
            runner = case['runner_suite']
            if runner is not None:
                # The entrypoint may still be planned: tools/coverage-report.py
                # counts only runners that exist, so a plan is never a result.
                fields(
                    runner, ('entrypoint', 'arguments', 'backend', 'repository', 'builder'), 'runner'
                )
                require(runner['backend'] == case['required_backend'], 'runner scope mismatch')
                project_path(runner['entrypoint'], 'runner entrypoint')
                require(runner['repository'] in ('driver', 'workspace'), 'runner repository')
                plain_line(runner['builder'], 'runner builder')
                require(
                    type(runner['arguments']) is list
                    and all(type(x) is str and '\0' not in x for x in runner['arguments']),
                    'runner arguments',
                )
            all_cases.append(case['id'])
        require(
            (node['required_backend'] == 'MIXED' and len(backends) > 1)
            or backends == {node['required_backend']},
            'aggregate backend scope',
        )

    for domain in catalog['domains']:
        fields(
            domain,
            ('id', 'prerequisites', 'required_milestones', 'requirements', 'criteria_ref'),
            'domain',
        )
        identifiers([domain['id']], 'domain')
        identifiers(domain['prerequisites'], 'domain prerequisites')
        identifiers(domain['required_milestones'], 'domain milestone dependencies')
        criteria_reference(domain['criteria_ref'], domain['id'])
        require(type(domain['requirements']) is list and domain['requirements'], 'requirements')
        counts['domains'] += 1
        for requirement in domain['requirements']:
            fields(
                requirement,
                ('id', 'mandatory', 'required_backend', 'cases', 'criteria_ref'),
                'requirement',
            )
            identifiers([requirement['id']], 'requirement')
            require(requirement['id'].startswith(domain['id'] + '.'), 'requirement namespace')
            require(type(requirement['mandatory']) is bool, 'mandatory must be boolean')
            suite(requirement)
            all_requirements.append(requirement['id'])
            counts['requirements'] += 1
            counts['mandatory_requirements'] += int(requirement['mandatory'])
            counts['case_families'] += len(requirement['cases'])
    identifiers(all_requirements, 'requirement')
    for milestone in catalog['integration_milestones']:
        fields(
            milestone,
            ('id', 'prerequisites', 'required_backend', 'closes_domains', 'cases', 'criteria_ref'),
            'milestone',
        )
        identifiers([milestone['id']], 'milestone')
        require(milestone['id'].startswith('integration.'), 'milestone namespace')
        require(milestone['closes_domains'] is False, 'milestone cannot close a domain')
        identifiers(milestone['prerequisites'], 'milestone prerequisites')
        suite(milestone)
    identifiers(all_cases, 'all case')
    case_nodes = [c for d in catalog['domains'] for r in d['requirements'] for c in r['cases']]
    case_nodes += [c for m in catalog['integration_milestones'] for c in m['cases']]
    graph(
        [
            {
                'id': c['id'],
                'prerequisites': [v for k, v in c['parameters'].items() if k.endswith('_case')],
            }
            for c in case_nodes
        ],
        'shared case parameters',
    )
    graph(catalog['domains'], 'domain')
    graph(catalog['integration_milestones'], 'milestone')
    domains = {d['id'] for d in catalog['domains']}
    milestones = {m['id'] for m in catalog['integration_milestones']}
    require(
        all(set(d['required_milestones']) <= milestones for d in catalog['domains']),
        'unknown domain milestone',
    )
    for key in ('optional_domains', 'assessment_domains'):
        identifiers(policy[key], key)
        require(set(policy[key]) <= domains, 'unknown ' + key)
    require(
        not set(policy['optional_domains']) & set(policy['assessment_domains']),
        'optional assessment conflict',
    )
    identifiers(catalog['required_for_release'], 'release')
    require(
        set(catalog['required_for_release']) == domains - set(policy['optional_domains']),
        'release domain coverage',
    )
    # Retired IDs stay listed with their reason; check-project refuses deletions.
    require(type(catalog['retired']) is list, 'retired list')
    live = (
        domains
        | milestones
        | {r['id'] for d in catalog['domains'] for r in d['requirements']}
        | set(all_cases)
    )
    for entry in catalog['retired']:
        fields(entry, ('id', 'kind', 'parent', 'reason', 'replaced_by'), 'retired entry')
        identifiers([entry['id']], 'retired')
        require(entry['id'] not in live, 'retired ID is live: ' + entry['id'])
        require(entry['kind'] in ('requirement', 'milestone', 'case'), 'retired kind')
        plain_line(entry['reason'], 'retired reason')
        identifiers(entry['replaced_by'], 'retired replacement')
        require(set(entry['replaced_by']) <= live, 'retired replacement must be live')
    identifiers([e['id'] for e in catalog['retired']], 'retired')
    for key in ('m1_domains', 'after_m1_domains'):
        identifiers(policy[key], key)
    require(
        not set(policy['m1_domains']) & set(policy['after_m1_domains'])
        and set(policy['m1_domains']) | set(policy['after_m1_domains']) == domains,
        'every domain is in M1 or after it, not both',
    )
    return counts


def validate_catalog(catalog):
    return validate_catalog_layout(catalog)


def validate_preserved_ids(catalog, baseline):
    """Protect existing acceptance identities while allowing new requirements."""
    requirements = {r['id']: r for d in catalog['domains'] for r in d['requirements']}
    milestones = {m['id']: m for m in catalog['integration_milestones']}
    retired = {e['id']: e['kind'] for e in catalog['retired']}
    for kind, current in (('requirements', requirements), ('milestones', milestones)):
        for ident, original in baseline[kind].items():
            if ident not in current:
                require(retired.get(ident) == kind[:-1], 'lost acceptance ID: ' + ident)
                require(
                    all(retired.get(c) == 'case' for c in original['cases']),
                    'lost case of a retired node: ' + ident,
                )
                continue
            node = current[ident]
            require(
                node['required_backend'] == original['required_backend'],
                'changed required scope: ' + ident,
            )
            if 'mandatory' in original:
                require(
                    node['mandatory'] == original['mandatory'],
                    'changed mandatory criterion: ' + ident,
                )
            cases = {c['id']: c['required_backend'] for c in node['cases']}
            require(
                all(
                    cases.get(c) == scope or c not in cases and retired.get(c) == 'case'
                    for c, scope in original['cases'].items()
                ),
                'lost or reparented case/scope: ' + ident,
            )


def validate_acceptance_documents(root, catalog):
    """Check cross-references and review bindings; never evaluate a hardware result."""
    nodes = [r for d in catalog['domains'] for r in d['requirements']]
    nodes += catalog['integration_milestones']
    references = [catalog['coverage_policy']['criteria_ref']]
    references += [d['criteria_ref'] for d in catalog['domains']]
    references += [n['criteria_ref'] for n in nodes]
    references += [c['criteria_ref'] for n in nodes for c in n['cases']]
    for ref in references:
        markdown_links(root, 'acceptance-reference.md', '[criteria](' + ref + ')')
    text = markdown_prose(ordinary(root, 'docs/checklist.md').read_text())
    rows = re.findall(r'^- \[([ xX])\] `([a-z0-9.-]+)`[^\n]*(?:\n(?:  [^\n]*|\n))*', text, re.M)
    expected = {n['id']: n for n in nodes}
    identifiers([i for _, i in rows], 'checklist requirement')
    require({i for _, i in rows} == set(expected), 'lost or unknown checklist requirement')
    # A checked box is an authored, reviewed assertion. Requiring a scoped
    # basis prevents an unbound mark; this checker cannot authenticate GPU work.
    for mark, ident in rows:
        if mark.lower() != 'x':
            continue
        block = re.search(
            r'^- \[[xX]\] `' + re.escape(ident) + r'`[^\n]*\n((?:  [^\n]*\n|\n)*)',
            text + '\n',
            re.M,
        )
        basis = re.search(r'Basis \(([^)]+)\): (\[[^\]]+\]\([^)]+\))', block[1] if block else '')
        require(basis is not None, 'checked requirement needs reviewed basis: ' + ident)
        scopes = {x.strip() for x in basis[1].split(',')}
        require(
            scopes == {c['required_backend'] for c in expected[ident]['cases']},
            'checked requirement scope mismatch: ' + ident,
        )
        markdown_links(root, 'docs/checklist.md', basis[2])


# QC-stale: names a comment or document gives that nothing in the tree defines.
OWN_NAME = re.compile(r'\b(?:DRM_IOCTL_POULSBACK|DRM_POULSBACK|POULSBACK|SGX535|poulsback|sgx535)_\w*')
COMMENT_CALL = re.compile(r'\b([a-z][a-z0-9]*_[a-z0-9_]+)\(')
CODE_PATH = re.compile(
    r'`((?:kernel|tools|tests|include|spec|support|docs|src/(?:compiler|raster|gallium|video|xorg))'
    r'/[\w./-]+)`'
)
C_TEXT = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'', re.S)
SCRIPT_SUFFIXES = {'.py', '.sh', '.build', '.mk', ''}


def quality_module(root):
    spec = importlib.util.spec_from_file_location('poulsback_quality', root / 'tools/quality.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def stale_names(root):
    """Hits {path:name or path:path: (1, detail)} of QC-stale (docs/tests.md#quality-gates)."""
    known, texts = set(), []
    for path, relative in project_files(root):
        if relative.startswith('benchmark/'):
            continue
        known.update((path.name, path.stem))
        try:
            text = path.read_text(encoding='utf-8')
        except UnicodeDecodeError:
            continue
        if path.suffix in ('.c', '.h', '.inc', '.cpp'):
            comments = []

            def code(match):
                value = match.group()
                if value.startswith(('//', '/*')):
                    comments.append(value)
                    return '\n' * value.count('\n')
                return value

            known.update(re.findall(r'\w+', C_TEXT.sub(code, text)))
            texts.append((relative, '\n'.join(comments), 'c'))
        elif path.suffix in ('.md', '.json', '.txt') and relative != 'quality-debts.txt':
            texts.append((relative, text, 'document'))
        elif path.suffix in SCRIPT_SUFFIXES or path.name == 'Makefile':
            lines = [re.sub(r'(?:^|\s)#.*$', '', line) for line in text.splitlines()]
            known.update(re.findall(r'\w+', '\n'.join(lines)))
            texts.append((relative, '\n'.join(re.findall(r'#.*$', text, re.M)), 'script'))
    hits = {}
    for relative, text, kind in texts:
        names = set(OWN_NAME.findall(text))
        if kind == 'c':
            names.update(COMMENT_CALL.findall(text))
        for name in names:
            if name in known or name.endswith('_') and any(k.startswith(name) for k in known):
                continue
            hits[f'{relative}:{name}'] = (1, 'names no symbol, string or file in the tree')
        if kind == 'document' and not relative.startswith('docs/re/'):
            for target in CODE_PATH.findall(text):
                target = target.rstrip('.,:;')
                if not any(c in target for c in '*{<') and not (root / target).exists():
                    hits[f'{relative}:{target}'] = (1, 'names a path that does not exist')
    return hits


def validate_stale(root):
    quality = quality_module(root)
    debts, closed = quality.load_debts(root)
    errors = quality.compare(['stale'], {'stale': stale_names(root)}, debts, closed)
    require(not errors, 'QC-stale:\n' + '\n'.join(errors))


def validate_ledger(root):
    """M1's rule: no case without a runner or criteria (tools/coverage-report.py)."""
    import importlib.util

    path = root / 'tools/coverage-report.py'
    spec = importlib.util.spec_from_file_location('coverage_report', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.check(module.Ledger(root, None))


def check(root):
    catalog = parse(ordinary(root, 'spec/dod.json').read_bytes())
    counts = validate_catalog(catalog)
    baseline = parse(ordinary(root, 'tests/project/acceptance_ids.json').read_bytes())
    validate_preserved_ids(catalog, baseline)
    validate_acceptance_documents(root, catalog)
    # The case lists and exclusions themselves: tools/coverage-report.py check.
    ordinary(root, catalog['coverage_policy']['exclusions_ref'])
    for domain in catalog['domains']:
        for case in (c for r in domain['requirements'] for c in r['cases']):
            if case['case_list'] is not None:
                ordinary(root, case['case_list'])
    for case in (c for m in catalog['integration_milestones'] for c in m['cases']):
        if case['case_list'] is not None:
            ordinary(root, case['case_list'])
    validate_ledger(root)
    for path, relative in project_files(root):
        raw = path.read_bytes()
        if b'\0' in raw:
            continue
        try:
            text = raw.decode('utf-8')
        except UnicodeDecodeError:
            continue
        scan_text(relative, text)
        if path.suffix == '.md':
            markdown_anchors(text)
            markdown_links(root, relative, text)
    documentation_routes(root)
    tool_documentation(root)
    validate_stale(root)
    return dict(
        status='PASS_DOCUMENTS_AND_CATALOG',
        counts=counts,
        contract_sha256=contract_sha(catalog),
        integration_counts=dict(
            milestones=len(catalog['integration_milestones']),
            case_families=sum(len(m['cases']) for m in catalog['integration_milestones']),
        ),
        run_results_evaluated=False,
        hardware_capability_promoted=False,
        private_corpus_required=False,
        target_actions=False,
    )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('check',))
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args(argv)
    try:
        report = check(args.root.resolve())
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(json.dumps(dict(status='INVALID', error=str(error))))
        return 1
    print(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
