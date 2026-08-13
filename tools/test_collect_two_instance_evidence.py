#!/usr/bin/env python3
"""Regression tests for M7 two-instance evidence classification."""

from __future__ import annotations

import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import collect_two_instance_evidence as evidence


def event(direction: int, src_ip: str, src_port: int,
          dst_ip: str, dst_port: int) -> dict[str, object]:
    return {
        "direction": direction,
        "src_ip": src_ip,
        "src_port": src_port,
        "dst_ip": dst_ip,
        "dst_port": dst_port,
        "payload_len": 24,
    }


def instance(label: str, ip: str,
             udp_events: list[dict[str, object]]) -> dict[str, object]:
    return {
        "label": label,
        "kind_counts": {"backend_error": 0},
        "truncation_risk": [],
        "kinds": {
            "dns_query": [
                event(0, ip, 40000, "178.62.43.212", 53),
            ],
            "tcp_open": [],
            "tcp_packet": [],
            "udp_packet": udp_events,
            "dhcp": [],
        },
        "udp_summary": evidence.summarize_udp(udp_events),
    }


def report_for(a_udp: list[dict[str, object]],
               b_udp: list[dict[str, object]]) -> dict[str, object]:
    instances = [
        instance("A", "192.168.4.32", a_udp),
        instance("B", "192.168.4.33", b_udp),
    ]
    return {
        "instances": instances,
        "correlation": evidence.correlate_instances(instances),
    }


class M7TransportVerdictTest(unittest.TestCase):
    def test_bidirectional_direct_udp_wins_even_without_retained_natneg(self):
        report = report_for(
            [event(0, "192.168.4.32", 42000, "192.168.4.33", 43000)],
            [event(0, "192.168.4.33", 43000, "192.168.4.32", 42000)],
        )

        verdict = evidence.m7_transport_verdict(report)

        self.assertEqual(
            verdict["status"],
            "direct_client_udp_bidirectional_observed",
        )
        self.assertEqual(verdict["natneg_udp_count"], 0)
        self.assertTrue(verdict["direct_client_udp_bidirectional"])

    def test_one_way_direct_udp_is_not_masked_by_missing_natneg(self):
        report = report_for(
            [event(0, "192.168.4.32", 42000, "192.168.4.33", 43000)],
            [],
        )

        verdict = evidence.m7_transport_verdict(report)

        self.assertEqual(verdict["status"], "direct_client_udp_one_way")
        self.assertEqual(verdict["natneg_udp_count"], 0)

    def test_natneg_without_peer_udp_still_reports_natneg_only(self):
        report = report_for(
            [event(0, "192.168.4.32", 42000, "95.217.77.181", 27901)],
            [],
        )

        verdict = evidence.m7_transport_verdict(report)

        self.assertEqual(verdict["status"], "natneg_without_peer_udp")
        self.assertEqual(verdict["natneg_udp_count"], 1)
        self.assertEqual(verdict["candidate_peer_udp_count"], 0)

    def test_acceptance_requires_transport_and_race_confirmation(self):
        report = report_for(
            [event(0, "192.168.4.32", 42000, "192.168.4.33", 43000)],
            [event(0, "192.168.4.33", 43000, "192.168.4.32", 42000)],
        )
        report["m7_transport_verdict"] = evidence.m7_transport_verdict(report)
        report["operator_confirmation"] = {
            "race_entry_operator_confirmed": True,
        }

        verdict = evidence.m7_acceptance_verdict(report)

        self.assertEqual(
            verdict["status"],
            "race_entry_confirmed_with_bidirectional_peer_udp",
        )

    def test_acceptance_does_not_treat_transport_as_race_entry(self):
        report = report_for(
            [event(0, "192.168.4.32", 42000, "192.168.4.33", 43000)],
            [event(0, "192.168.4.33", 43000, "192.168.4.32", 42000)],
        )
        report["m7_transport_verdict"] = evidence.m7_transport_verdict(report)
        report["operator_confirmation"] = {
            "race_entry_operator_confirmed": False,
        }

        verdict = evidence.m7_acceptance_verdict(report)

        self.assertEqual(
            verdict["status"],
            "transport_observed_waiting_for_race_entry",
        )


if __name__ == "__main__":
    unittest.main()
