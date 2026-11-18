# Input performance candidate

> Historical development note. The pre-signing status below describes the candidate stage. The signed 1.4.22.0 release is now published; see [release validation](DEMO-VALIDATION.md) and the [public runtime benchmark records](https://titancsa.github.io/TitanCSA-Benchmarks/interceptor/results/). The older event-cost figures below are local software fixtures, not end-to-end input latency.

During development, Interceptor 1.4.22.0 was a local source candidate awaiting its
matching Microsoft-signed driver CAB. Existing embedded signed payloads were
retained until that CAB returned; they were not the candidate driver. The trial
remained one hour per Windows boot.

The SDK uses one grouped readiness check after sparse input and individual checks
after several families were ready. The previous result is a thread-safe hint;
Windows determines current readiness on every call. Plain wire vector reads also
use bulk assignment for batches containing more than one input.

The following measurements used private Windows events with all six families
owned. Each value is the median of seven alternating baseline/candidate runs;
each run used seven samples of 50,000 calls.

| Ready families | Previous event cost | Candidate event cost |
| --- | ---: | ---: |
| Keyboard only | 2.120 microseconds | 1.088 microseconds |
| Mouse only | 1.772 microseconds | 1.047 microseconds |
| HID only | 1.470 microseconds | 0.938 microseconds |
| All six | 2.074 microseconds | 2.073 microseconds |

These are software event-processing costs. They exclude physical device, USB,
driver capture and downstream input delivery, and establish no zero-latency claim.
The keyboard/mouse edition owns only two streams and is not eligible for this
grouped-tail gain. The controller edition owns four streams; these six-stream
figures are not measurements of that edition.

Benchmarks remain in separate validation fixtures. The production SDK contains
no benchmark hooks. Repeated single mouse sends already use one send IOCTL per
call; this candidate retains their existing send path.
