# Security policy

Thank you for helping keep Poulsback and its users safe.

Send suspected vulnerabilities privately to **Alex** at
[alex@kmn.io](mailto:alex@kmn.io), with a subject such as
`[poulsback security] brief description`.

Please give us an opportunity to review the finding and coordinate disclosure
before posting details publicly.

If you are unsure whether a finding is security-sensitive, email is a good place
to start.

## Scope and maintenance

Reports about Poulsback's kernel module, UAPI, GPU memory isolation, Mesa
driver, Xorg DDX, libraries and repository build tools are welcome.

Examples include unauthorized memory access, information disclosure, privilege
escalation and failures that untrusted input can use to disrupt other clients or
the system.

Poulsback is experimental.

Fixes are developed against current code; there is no maintained stable-branch
or long-term security-backport programme.

Please report findings in older public snapshots too, naming the exact revision.

You do not need to reproduce a suspected vulnerability on newer code or physical
hardware before reporting a source finding.

The [status pages](docs/status/README.md) describe implementation and
qualification limits.

Findings in external Linux, Mesa or vendor components may need coordination with
their maintainers; include the dependency identity so we can determine scope.

## What to include

- A description of the issue, affected component and suspected security impact.
- The release or commit, local changes and relevant build or runtime versions.
- Required privileges and access: for example, root, a DRM device node, a local
  graphics client or an input file.
- Reproduction steps, a small test case, or a source-level explanation. Include
  machine model and PCI ID/revision when the finding depends on hardware.
- Relevant logs or traces, and whether the behavior was observed or inferred.
- Your preferred contact and whether you want public credit, and under what name.

An initial explanation is enough to start a report.

Arrange transfer of large or sensitive attachments by email.

Remove unrelated personal information, credentials and other people's data.

Test only on systems you control or are authorized to test; reproducing damage
is not required to establish a useful report.

## Handling reports

The maintainer works with the reporter to understand the finding and coordinate
a fix and disclosure with affected upstream projects.

Response and fix timing depend on the finding and available maintenance
capacity.

We will agree with you how you would like to be credited before publishing
reporter credit.

For ordinary bug reports and questions, use [Support](SUPPORT.md).

A crash or hang can be security-relevant when untrusted input crosses a
protection boundary; report it privately when that is suspected.
