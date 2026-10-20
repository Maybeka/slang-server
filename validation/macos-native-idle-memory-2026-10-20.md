# Current candidate Mac memory evaluation

Candidate SHA-256: `c30d503050c7f499e30c27f36fd25dc0f058a11decb08ded4540baa53db624a9`.
Accepted reference: `8ee6e5d00c3b95c672779d24d5038866ec39a80ad6398d6e37acbe012dc68cac`.

All 180 main cells pass: three real projects, five arms, three independent
20-edit and three 1,000-edit sessions, separately in full-design and isolated
diagnostic-worker modes. Full-design enabled cells select exact RTL instances;
worker enabled cells select packages. All four raw audits pass. No failed or censored row
is omitted. Every matching-version diagnostic, error/repair, fallback/repair
and save-refresh assertion passes. Current-only manual frozen refresh remains
additional stress. All experiments remain disabled by default.

The separate matched UBUS controls add twelve successful cells: two native
arms, three 20-edit and three 1,000-edit paired repetitions, with no status
query or manual frozen-refresh command on either binary. Both raw audits pass.

Collection is sequential, with no build, editor, other test, bootstrap or
second sampler running concurrently. The 20 ms sampler records parent and
simultaneous child RSS and physical footprint separately, from before spawn
through postsequence queries, excluding the Python driver and orderly close.
Family sums can count shared pages and sampling can miss peaks. Three-repeat
medians/ranges are descriptive; they prove neither equivalence nor tail bounds.

## Thousand-edit all-phase peaks

All values are MiB, median [minimum, maximum] of all three repetitions.
The complete original peaks include the extra current-only refresh.

### Full design

| Case | Arm | Parent RSS | Family RSS | Parent footprint | Family footprint |
|---|---|---:|---:|---:|---:|
| official-ubus | accepted-native | 1514.7 [1304.1, 1548.8] | 1514.7 [1304.1, 1548.8] | 1565.4 [1501.7, 1566.7] | 1565.4 [1501.7, 1566.7] |
| official-ubus | accepted-uvm | 536.0 [535.7, 542.1] | 536.0 [535.7, 542.1] | 431.0 [424.3, 432.7] | 431.0 [424.3, 432.7] |
| official-ubus | current-frozen | 239.9 [239.0, 242.0] | 239.9 [239.0, 242.0] | 155.0 [154.9, 216.7] | 155.0 [154.9, 216.7] |
| official-ubus | current-native | 1596.3 [1472.9, 1597.6] | 1596.3 [1472.9, 1597.6] | 1555.0 [1542.3, 1573.1] | 1555.0 [1542.3, 1573.1] |
| official-ubus | current-uvm | 236.0 [235.7, 236.1] | 236.0 [235.7, 236.1] | 148.4 [147.9, 154.3] | 148.4 [147.9, 154.3] |
| spi-block | accepted-native | 1339.8 [1277.8, 2188.2] | 1339.8 [1277.8, 2188.2] | 1549.1 [1520.1, 2288.4] | 1549.1 [1520.1, 2288.4] |
| spi-block | accepted-uvm | 252.4 [221.1, 335.5] | 252.4 [221.1, 335.5] | 195.6 [172.3, 273.9] | 195.6 [172.3, 273.9] |
| spi-block | current-frozen | 224.5 [224.2, 235.0] | 224.5 [224.2, 235.0] | 139.1 [138.1, 209.0] | 139.1 [138.1, 209.0] |
| spi-block | current-native | 1331.7 [1280.0, 1566.8] | 1331.7 [1280.0, 1566.8] | 1544.8 [1536.0, 1563.4] | 1544.8 [1536.0, 1563.4] |
| spi-block | current-uvm | 219.8 [219.7, 227.0] | 219.8 [219.7, 227.0] | 138.5 [135.2, 160.2] | 138.5 [135.2, 160.2] |
| spi-pss | accepted-native | 1305.5 [1223.7, 1676.8] | 1305.5 [1223.7, 1676.8] | 1523.3 [1494.8, 1643.4] | 1523.3 [1494.8, 1643.4] |
| spi-pss | accepted-uvm | 258.5 [258.2, 269.5] | 258.5 [258.2, 269.5] | 155.0 [132.8, 168.5] | 155.0 [132.8, 168.5] |
| spi-pss | current-frozen | 241.7 [241.0, 248.5] | 241.7 [241.0, 248.5] | 117.8 [117.8, 141.1] | 117.8 [117.8, 141.1] |
| spi-pss | current-native | 1391.5 [1111.4, 1875.5] | 1391.5 [1111.4, 1875.5] | 1603.8 [1559.0, 1986.6] | 1603.8 [1559.0, 1986.6] |
| spi-pss | current-uvm | 239.9 [237.3, 243.3] | 239.9 [237.3, 243.3] | 136.4 [117.9, 142.5] | 136.4 [117.9, 142.5] |

### Diagnostic-worker family

| Case | Arm | Parent RSS | Family RSS | Parent footprint | Family footprint |
|---|---|---:|---:|---:|---:|
| W01-compare | accepted-native | 1636.3 [1496.6, 2083.0] | 1725.0 [1593.6, 2178.0] | 1585.6 [1506.6, 2033.1] | 1661.9 [1588.3, 2114.7] |
| W01-compare | accepted-uvm | 163.6 [140.2, 166.0] | 273.7 [250.4, 275.9] | 107.2 [86.3, 113.5] | 170.8 [146.8, 179.0] |
| W01-compare | current-frozen | 135.0 [130.9, 136.5] | 250.1 [242.0, 250.3] | 94.1 [88.5, 102.8] | 157.7 [149.3, 175.9] |
| W01-compare | current-native | 1595.2 [1520.9, 1787.6] | 1691.4 [1618.3, 1883.8] | 1556.1 [1461.5, 1742.0] | 1643.1 [1543.3, 1829.0] |
| W01-compare | current-uvm | 158.3 [148.7, 160.2] | 268.7 [259.1, 270.5] | 110.3 [80.9, 113.2] | 169.5 [153.8, 173.9] |
| W07-pss-multidoc | accepted-native | 1600.3 [1523.0, 1665.8] | 1699.6 [1621.1, 1764.0] | 1549.4 [1459.9, 1613.0] | 1632.6 [1543.8, 1691.0] |
| W07-pss-multidoc | accepted-uvm | 203.5 [196.2, 203.5] | 314.6 [307.9, 314.7] | 142.1 [137.1, 148.8] | 175.3 [170.4, 182.0] |
| W07-pss-multidoc | current-frozen | 188.7 [188.3, 193.4] | 299.0 [298.3, 303.1] | 129.3 [118.3, 131.0] | 162.7 [150.1, 176.2] |
| W07-pss-multidoc | current-native | 1704.7 [1677.6, 1950.2] | 1804.1 [1777.4, 1975.1] | 1653.3 [1630.3, 1899.5] | 1727.0 [1713.2, 1909.7] |
| W07-pss-multidoc | current-uvm | 210.4 [197.2, 215.7] | 321.8 [309.1, 327.1] | 150.9 [125.8, 153.8] | 184.3 [159.1, 187.1] |
| W08-ubus-task | accepted-native | 1623.7 [1614.5, 1637.1] | 1719.9 [1697.7, 1725.3] | 1579.5 [1570.0, 1582.0] | 1658.7 [1644.2, 1669.1] |
| W08-ubus-task | accepted-uvm | 163.5 [162.9, 165.3] | 276.7 [276.0, 278.2] | 107.7 [104.4, 110.5] | 183.0 [182.8, 183.4] |
| W08-ubus-task | current-frozen | 150.3 [147.2, 153.8] | 266.3 [264.3, 270.9] | 106.5 [103.7, 112.0] | 185.3 [183.4, 188.7] |
| W08-ubus-task | current-native | 2143.8 [1756.5, 2144.5] | 2221.0 [1851.1, 2221.6] | 2090.6 [1714.8, 2100.1] | 2153.8 [1800.1, 2165.1] |
| W08-ubus-task | current-uvm | 164.6 [164.3, 167.5] | 277.7 [277.4, 280.6] | 113.4 [107.2, 114.0] | 183.6 [182.7, 186.0] |

## Feature-off investigation

The original observed matrix uses status queries and a final manual frozen
refresh on the current binary only; the accepted executable does not
implement these commands. A common-sequence endpoint cuts at save-refresh
but still contains unequal status instrumentation. Original all-phase
peaks remain authoritative observations, not discarded outliers.

| Mode / case | Accepted family RSS | Current family RSS | Change |
|---|---:|---:|---:|
| full / official-ubus | 1514.7 | 1596.3 | +5.39% |
| full / spi-block | 1339.8 | 1331.7 | -0.60% |
| full / spi-pss | 1305.5 | 1391.5 | +6.59% |
| worker / W01-compare | 1725.0 | 1691.4 | -1.95% |
| worker / W07-pss-multidoc | 1699.6 | 1804.1 | +6.15% |
| worker / W08-ubus-task | 1719.9 | 2221.0 | +29.14% |

Matched worker UBUS, both native experiments off, with the same commands:

| Length | Metric | Accepted median | Current median | Change |
|---|---|---:|---:|---:|
| short | peakParentRss | 200.6 | 200.6 | +0.02% |
| short | peakFamilyRss | 271.5 | 276.0 | +1.65% |
| short | peakParentFootprint | 166.2 | 165.8 | -0.25% |
| short | peakFamilyFootprint | 226.9 | 227.7 | +0.36% |
| long | peakParentRss | 1705.5 | 1679.4 | -1.53% |
| long | peakFamilyRss | 1803.8 | 1776.6 | -1.51% |
| long | peakParentFootprint | 1650.6 | 1624.2 | -1.60% |
| long | peakFamilyFootprint | 1739.8 | 1712.1 | -1.60% |

The matched long family-RSS median is 1776.6 MiB current versus 1803.8 MiB
accepted (-1.51%); physical footprint is 1712.1 versus 1739.8 MiB (-1.60%).
Individual paired RSS differences are -519.0, +56.4 and +129.6 MiB. The
instrumented matrix's +29.14% increase is not reproduced by the matched native
sequence. This satisfies the required investigation, not an equivalence or
memory-saving claim: three observations, unequal instrumentation in the original
matrix and substantial run variability do not establish a causal explanation.
No runtime fix is justified by these samples alone; both native routes still
consume substantial memory over 1,000 edits. Preserve the original stress result
and all matched observations. New cache strategies remain opt-in, with actual
retirement and configured retention budgets independently verified.

Status computes file-cache usage and reads source-location table size/capacity.
It does not scan every location entry. Extra manual refresh cannot by itself
explain a peak already observed earlier. The matched controls isolate those
command differences; they do not identify an allocator or compiler root cause.

## Lifecycle and scope

Each generic 1,000-edit session records actual parent context retirement:
8 times for SPI, 8 for PSS and 31 for UBUS in every repetition and mode.
Sampled location-entry counts reset. Parent retained cache/status charges
stay within the configured 64 MiB. A supplemental audit checks every sampled
generic mark individually: full-design syntax <=48 MiB and values <=16 MiB,
exploration syntax <=64 MiB (513 marks across short/long). It does not expose a
child retained-byte counter. This is a retention budget, not a total
RSS cap. Worker compiler state is separately owned and bounded by its
runtime budget; family sampling includes the real child. Current manual
refresh proves renewed generic preparation, without issuing unsupported
commands to the accepted executable. Full-design traces contain one process;
worker traces observe at least two.

Full-design cells use exact instance selectors, compare complete hierarchy
values after each edit and require actual cross-edit value reuse. Worker
cells use selected packages. Together they cover the current three-repeat
thousand-edit instance/package lifetime gates. The historical instance
memory pilot retains its original candidate, sampling window and limits.

## Evidence

All raw inputs and analyses remain under
`untracked/frozen-registry-2026-10-20/`. Compact audit identities:

| Audit | SHA-256 |
|---|---|
| `native-idle-full-memory-short-audit.json` | `1a8116a5debdba7780f34b24a1ff6899ddcbbd9872527ec1e50628ad47abe75a` |
| `native-idle-full-memory-long-audit.json` | `e0e8847024ab3badb84eb92151b96baf814dc91b3e95c0dddf82f4048c7f85b9` |
| `native-idle-worker-memory-short-audit.json` | `ea7b2bae97097036dd5fa9565723e979700bd313bd4802dfb8998d2b09e14908` |
| `native-idle-worker-memory-long-audit.json` | `f931a3728905998e7cf5676d24c350bbfb3879173483bf85e9f5a87554a8a64d` |
| `native-memory-controls-short-audit.json` | `c40e6cebc424ed89acd73f566da769aeca49056f5aa6d4709eedac206ddf16ab` |
| `native-memory-controls-long-audit.json` | `6a58d483b5c347f27646f5b362d92ab8cba4df2444839f7d5f35cd9ac1c1974f` |
