package tuners_test

import (
	"fmt"
	"testing"
	"time"
	"vectorized/pkg/os"
	"vectorized/pkg/tuners"
	"vectorized/pkg/tuners/ethtool"
	"vectorized/pkg/tuners/executors"
	"vectorized/pkg/tuners/hwloc"
	"vectorized/pkg/tuners/irq"
	"vectorized/pkg/tuners/network"
	"vectorized/pkg/utils"

	"github.com/spf13/afero"
	"github.com/stretchr/testify/require"
)

func mockNetTunersFactory(
	fs afero.Fs, exec executors.Executor,
) (tuners.NetTunersFactory, error) {
	procFile := irq.NewProcFile(fs)
	proc := os.NewProc()
	timeout := 1 * time.Second
	hwlocCmd := hwloc.NewHwLocCmd(proc, timeout)
	eth, err := ethtool.NewEthtoolWrapper()
	if err != nil {
		return nil, err
	}
	return tuners.NewNetTunersFactory(
		fs,
		procFile,
		irq.NewDeviceInfo(fs, procFile),
		eth,
		irq.NewBalanceService(fs, proc, exec, timeout),
		irq.NewCpuMasks(fs, hwlocCmd, exec),
		exec,
	), nil
}

func TestSynBacklogTuner(t *testing.T) {
	tests := []struct {
		name           string
		before         func(fs afero.Fs) error
		expectChange   bool
		expected       int
		expectedErrMsg string
	}{
		{
			name: "it shouldn't do anything if current >= reference",
			before: func(fs afero.Fs) error {
				_, err := utils.WriteBytes(
					fs,
					[]byte("20000000"),
					network.SynBacklogFile,
				)
				return err
			},
		},
		{
			name: "it shouldn't do anything if current == reference",
			before: func(fs afero.Fs) error {
				_, err := utils.WriteBytes(
					fs,
					[]byte("4096"),
					network.SynBacklogFile,
				)
				return err
			},
		},
		{
			name: "it should set the value if current < reference",
			before: func(fs afero.Fs) error {
				_, err := utils.WriteBytes(
					fs,
					[]byte("12"),
					network.SynBacklogFile,
				)
				return err
			},
			expectChange: true,
			expected:     4096,
		},
		{
			name:           "it should fail if the file is missing",
			expectedErrMsg: network.SynBacklogFile,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(st *testing.T) {
			const scriptPath = "/tune.sh"
			fs := afero.NewMemMapFs()
			exec := executors.NewScriptRenderingExecutor(fs, scriptPath)
			if tt.before != nil {
				err := tt.before(fs)
				require.NoError(st, err)
			}
			f, err := mockNetTunersFactory(fs, exec)
			require.NoError(st, err)
			tuner := f.NewSynBacklogTuner()
			res := tuner.Tune()
			if tt.expectedErrMsg != "" {
				require.Contains(st, res.Error().Error(), tt.expectedErrMsg)
				return
			}
			require.NoError(st, res.Error())
			contents, err := afero.ReadFile(fs, scriptPath)
			require.NoError(st, err)
			expected := `#!/bin/bash

# Redpanda Tuning Script
# ----------------------------------
# This file was autogenerated by RPK

`
			if tt.expectChange {
				expected = expected + fmt.Sprintf(`echo '%d' > %s
`,
					tt.expected,
					network.SynBacklogFile,
				)
			}
			require.Exactly(st, expected, string(contents))
		})
	}
}

func TestListenBacklogTuner(t *testing.T) {
	tests := []struct {
		name           string
		before         func(fs afero.Fs) error
		expectChange   bool
		expected       int
		expectedErrMsg string
	}{
		{
			name: "it shouldn't do anything if current >= reference",
			before: func(fs afero.Fs) error {
				_, err := utils.WriteBytes(
					fs,
					[]byte("20000000"),
					network.ListenBacklogFile,
				)
				return err
			},
		},
		{
			name: "it shouldn't do anything if current == reference",
			before: func(fs afero.Fs) error {
				_, err := utils.WriteBytes(
					fs,
					[]byte("4096"),
					network.ListenBacklogFile,
				)
				return err
			},
		},
		{
			name: "it should set the value if current < reference",
			before: func(fs afero.Fs) error {
				_, err := utils.WriteBytes(
					fs,
					[]byte("12"),
					network.ListenBacklogFile,
				)
				return err
			},
			expectChange: true,
			expected:     4096,
		},
		{
			name:           "it should fail if the file is missing",
			expectedErrMsg: network.ListenBacklogFile,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(st *testing.T) {
			const scriptPath = "/tune.sh"
			fs := afero.NewMemMapFs()
			exec := executors.NewScriptRenderingExecutor(fs, scriptPath)
			if tt.before != nil {
				err := tt.before(fs)
				require.NoError(st, err)
			}
			f, err := mockNetTunersFactory(fs, exec)
			require.NoError(st, err)
			tuner := f.NewListenBacklogTuner()
			res := tuner.Tune()
			if tt.expectedErrMsg != "" {
				require.Contains(st, res.Error().Error(), tt.expectedErrMsg)
				return
			}
			require.NoError(st, res.Error())
			contents, err := afero.ReadFile(fs, scriptPath)
			require.NoError(st, err)
			expected := `#!/bin/bash

# Redpanda Tuning Script
# ----------------------------------
# This file was autogenerated by RPK

`
			if tt.expectChange {
				expected = expected + fmt.Sprintf(`echo '%d' > %s
`,
					tt.expected,
					network.ListenBacklogFile,
				)
			}
			require.Exactly(st, expected, string(contents))
		})
	}
}
