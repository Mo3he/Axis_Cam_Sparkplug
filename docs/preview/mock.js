// Fictional broker, status, metric and device-event data; refresh the copied app UI and sample API responses from its device UI when it changes.
"use strict";

let sampleSettings = {
  enabled: true,
  group_id: "ExampleBuilding",
  edge_node_id: "SampleCamera01",
  device_id: "EntryCamera",
  primary_host_id: "SampleBroker",
  spec_version: "3.0",
  host: "broker.example.invalid",
  port: 1883,
  alternates: [],
  client_id: "sample-edge-node",
  username: "sample-user",
  password_set: false,
  keepalive: 60,
  tls: false,
  tls_verify: true,
  effective: { port: 1883, edge_node_id: "SampleCamera01", client_id: "sample-edge-node" },
};

let sampleMetrics = [
  {
    name: "System/CPU usage",
    datatype: "Float",
    writable: false,
    invert: false,
    event: { topic: ["tns1:Device", "tnsaxis:SampleMetrics"], source_key: "", source_value: "", data_key: "CpuPercent" },
  },
  {
    name: "System/Memory usage",
    datatype: "Float",
    writable: false,
    invert: false,
    event: { topic: ["tns1:Device", "tnsaxis:SampleMetrics"], source_key: "", source_value: "", data_key: "MemoryPercent" },
  },
  {
    name: "Environment/Temperature",
    datatype: "Float",
    writable: false,
    invert: false,
    event: { topic: ["tns1:Device", "tnsaxis:SampleEnvironment"], source_key: "", source_value: "", data_key: "TemperatureC" },
  },
];

const liveMetrics = [
  { name: "System/CPU usage", value: "18.6 %", subscribed: true, writable: false, last_event_time: 1795680000, last_event_value: "18.6" },
  { name: "System/Memory usage", value: "42.1 %", subscribed: true, writable: false, last_event_time: 1795680000, last_event_value: "42.1" },
  { name: "Environment/Temperature", value: "21.8 °C", subscribed: true, writable: false, last_event_time: 1795680000, last_event_value: "21.8" },
];
let sampleTls = { ca: [], cert: [], key: null };

const sampleEvents = `<?xml version="1.0" encoding="utf-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
  xmlns:wsnt="http://docs.oasis-open.org/wsn/t-1"
  xmlns:wstop="http://docs.oasis-open.org/wsn/t-1"
  xmlns:tns1="http://www.onvif.org/ver10/topics"
  xmlns:tnsaxis="http://www.axis.com/2009/event/topics"
  xmlns:aev="http://www.axis.com/vapix/ws/event1"
  xmlns:tt="http://www.axis.com/vapix/ws/event1">
  <s:Body><wsnt:TopicSet>
    <tns1:Device>
      <tnsaxis:SampleMetrics wstop:topic="true" aev:NiceName="Sample device metrics">
        <tt:MessageInstance>
          <tt:DataInstance>
            <tt:SimpleItemInstance Name="CpuPercent" Type="xsd:float"><tt:Value>18.6</tt:Value></tt:SimpleItemInstance>
            <tt:SimpleItemInstance Name="MemoryPercent" Type="xsd:float"><tt:Value>42.1</tt:Value></tt:SimpleItemInstance>
          </tt:DataInstance>
        </tt:MessageInstance>
      </tnsaxis:SampleMetrics>
      <tnsaxis:SampleEnvironment wstop:topic="true" aev:NiceName="Sample environment">
        <tt:MessageInstance>
          <tt:DataInstance><tt:SimpleItemInstance Name="TemperatureC" Type="xsd:float"><tt:Value>21.8</tt:Value></tt:SimpleItemInstance></tt:DataInstance>
        </tt:MessageInstance>
      </tnsaxis:SampleEnvironment>
    </tns1:Device>
  </wsnt:TopicSet></s:Body>
</s:Envelope>`;

function jsonResponse(body, status = 200) {
  return new Response(JSON.stringify(body), { status, headers: { "Content-Type": "application/json" } });
}

window.fetch = async (input, init = {}) => {
  const url = new URL(typeof input === "string" ? input : input.url, location.href);
  if (url.origin !== location.origin)
    return new Response("Offline preview blocks external requests.", { status: 403 });
  if (url.pathname.endsWith("/vapix/services")) return new Response(sampleEvents, { headers: { "Content-Type": "application/soap+xml" } });
  const routeMatch = url.pathname.match(/\/api\/(.*)$/);
  const route = routeMatch && routeMatch[1];
  if (!route) return new Response("Offline preview: this endpoint is not available.", { status: 404 });

  if (route === "settings" && (!init.method || init.method === "GET")) return jsonResponse(sampleSettings);
  if (route === "settings" && init.method === "POST") {
    const submitted = JSON.parse(init.body || "{}");
    sampleSettings = {
      ...sampleSettings,
      ...submitted,
      password_set: Boolean(submitted.password) || sampleSettings.password_set,
      password: undefined,
      effective: {
        port: Number(submitted.port) || 1883,
        edge_node_id: submitted.edge_node_id || "SampleCamera01",
        client_id: submitted.client_id || "sample-edge-node",
      },
    };
    return jsonResponse({ ok: true });
  }
  if (route === "metrics" && (!init.method || init.method === "GET")) return jsonResponse({ metrics: sampleMetrics });
  if (route === "metrics" && init.method === "POST") {
    sampleMetrics = JSON.parse(init.body || "{}").metrics || sampleMetrics;
    return jsonResponse({ ok: true });
  }
  if (route === "tls" && (!init.method || init.method === "GET")) return jsonResponse(sampleTls);
  if (route === "tls" && init.method === "POST") {
    sampleTls = {
      ca: [{ subject: "Fictional Example CA" }],
      cert: [{ subject: "Sample Client Certificate" }],
      key: { type: "RSA", bits: 2048 },
    };
    return jsonResponse({ ok: true });
  }
  if (route === "tls/delete" && init.method === "POST") {
    sampleTls = { ca: [], cert: [], key: null };
    return jsonResponse({ ok: true });
  }
  if (route === "status") {
    return jsonResponse({
      enabled: sampleSettings.enabled,
      version: "Offline sample",
      sparkplug: {
        online: true,
        host_online: true,
        group_id: sampleSettings.group_id,
        edge_node_id: sampleSettings.edge_node_id,
        device_id: sampleSettings.device_id,
        bdseq: 4,
        seq: 128,
        data_published: 128,
        commands_received: 2,
        buffered: 0,
      },
      mqtt: { connected: true, active_host: sampleSettings.host, active_port: sampleSettings.port, use_tls: false },
      metrics: liveMetrics,
    });
  }
  return new Response("Offline preview: this endpoint is not available.", { status: 404 });
};
