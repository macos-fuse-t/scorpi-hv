# ARM64 HVF Hyper-V features

Configure Hyper-V features under `cpu.hyperv` in the hypervisor YAML:

```yaml
cpu:
  cores: 4
  nested-virt: false
  hyperv:
    crash: true     # default: true
    stimers: false  # default: false
```

| Feature | Default | Behavior |
| --- | --- | --- |
| `crash` | `true` | Writes guest crash notifications and bugcheck parameters to the VM log. |
| `stimers` | `false` | Enables synthetic timers and the supporting clock and timer-message services. |

Hyper-V is active when either feature is enabled. Set both to false to disable
it. There is no separate activation setting. Feature values must be `true` or
`false`.

Discovery, virtual CPU identification and Windows boot integration are available
whenever Hyper-V is active. The timer feature includes a shared reference clock,
four timers per virtual CPU, one-shot and periodic operation, and interrupt and
message delivery.

These features require the ARM64 HVF backend with nested virtualization disabled.
Changes apply when the VM is started; fully stop and start an existing VM to
apply them. Scorpi's manager does not yet expose these fields in generated
runtime YAML.

Windows CPU sampling additionally requires matching Scorpi EDK2 firmware and the
[Windows HAL extension](../platform/windows/HalExtScorpiTimer/README.md).
