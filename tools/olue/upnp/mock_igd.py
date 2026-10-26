"""REQ-UPNP: a pretend UPnP router on this machine, for testing the game's port mapping without touching a real router.

Answers an M-SEARCH sent to 127.0.0.1:<ssdp port> (the game's -OLUpnpSsdp=127.0.0.1:<ssdp port>), serves a device
description with a WANIPConnection service, and answers GetExternalIPAddress, AddPortMapping and DeletePortMapping.
Every call is written as one JSON line to --log, with the mapping table after it.

    py -3 mock_igd.py --log calls.jsonl [--ssdp 19001] [--http 19002] [--external 203.0.113.7]
                      [--refuse] [--permanent-only] [--chunked]

--refuse answers AddPortMapping with UPnP error 718 (ConflictInMappingEntry); --permanent-only answers a mapping with a
lease with 725 (OnlyPermanentLeasesSupported); --chunked sends the description chunked; --external 100.64.3.4 plays a
carrier-grade NAT router.  Listens on 127.0.0.1 only.
"""
import argparse
import json
import re
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SERVICE = "urn:schemas-upnp-org:service:WANIPConnection:1"

DESC = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
<specVersion><major>1</major><minor>0</minor></specVersion>
<device><deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType><friendlyName>Mock IGD</friendlyName>
<serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType><controlURL>/ctl/L3F</controlURL></service></serviceList>
<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
<serviceList><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
<serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId><controlURL>/ctl/IPConn</controlURL>
<eventSubURL>/evt/IPConn</eventSubURL><SCPDURL>/WANIPCn.xml</SCPDURL></service></serviceList>
</device></deviceList></device></deviceList></device></root>
"""

mappings = {}
lock = threading.Lock()
args = None


def log(entry):
    entry["t"] = round(time.time(), 3)
    with lock:
        entry["mappings"] = {f"{k[0]}/{k[1]}": v for k, v in mappings.items()}
        line = json.dumps(entry)
        print(line, flush=True)
        with open(args.log, "a", encoding="utf-8") as f:
            f.write(line + "\n")


def arg(body, name):
    m = re.search(r"<(?:\w+:)?%s>(.*?)</(?:\w+:)?%s>" % (name, name), body, re.S)
    if m:
        return m.group(1).strip()
    return "" if re.search(r"<(?:\w+:)?%s\s*/>" % name, body) else None


def soap_ok(action, inner=""):
    return ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
            f'<u:{action}Response xmlns:u="{SERVICE}">{inner}</u:{action}Response></s:Body></s:Envelope>')


def soap_fault(code, desc):
    return ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><s:Fault><faultcode>s:Client</faultcode>'
            '<faultstring>UPnPError</faultstring><detail><UPnPError xmlns="urn:schemas-upnp-org:control-1-0">'
            f'<errorCode>{code}</errorCode><errorDescription>{desc}</errorDescription></UPnPError></detail></s:Fault>'
            '</s:Body></s:Envelope>')


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send_body(self, status, body, chunked=False):
        data = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", 'text/xml; charset="utf-8"')
        if chunked:
            self.protocol_version = "HTTP/1.1"
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            for i in range(0, len(data), 200):
                part = data[i:i + 200]
                self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    def do_GET(self):
        log({"call": "GET", "path": self.path})
        if self.path == "/rootDesc.xml":
            self.send_body(200, DESC, chunked=args.chunked)
        else:
            self.send_body(404, "")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n).decode("utf-8", "replace")
        action = (self.headers.get("SOAPAction", "").strip('"').split("#") + [""])[1]
        entry = {"call": action, "path": self.path, "soapaction": self.headers.get("SOAPAction", "")}
        if self.path != "/ctl/IPConn":
            log(dict(entry, result="404"))
            return self.send_body(404, "")
        if action == "GetExternalIPAddress":
            log(dict(entry, result="ok", external=args.external))
            return self.send_body(200, soap_ok(action, f"<NewExternalIPAddress>{args.external}</NewExternalIPAddress>"))
        if action == "AddPortMapping":
            fields = {k: arg(body, k) for k in ("NewRemoteHost", "NewExternalPort", "NewProtocol", "NewInternalPort",
                                                "NewInternalClient", "NewEnabled", "NewPortMappingDescription", "NewLeaseDuration")}
            entry["args"] = fields
            if any(v is None for v in fields.values()):
                log(dict(entry, result="402 missing argument"))
                return self.send_body(500, soap_fault(402, "Invalid Args"))
            if args.refuse:
                log(dict(entry, result="718"))
                return self.send_body(500, soap_fault(718, "ConflictInMappingEntry"))
            if args.permanent_only and fields["NewLeaseDuration"] != "0":
                log(dict(entry, result="725"))
                return self.send_body(500, soap_fault(725, "OnlyPermanentLeasesSupported"))
            with lock:
                mappings[(fields["NewProtocol"], fields["NewExternalPort"])] = {
                    "client": fields["NewInternalClient"], "port": fields["NewInternalPort"],
                    "lease": fields["NewLeaseDuration"], "desc": fields["NewPortMappingDescription"]}
            log(dict(entry, result="ok"))
            return self.send_body(200, soap_ok(action))
        if action == "DeletePortMapping":
            key = (arg(body, "NewProtocol"), arg(body, "NewExternalPort"))
            with lock:
                had = mappings.pop(key, None)
            log(dict(entry, result="ok" if had else "714", key=f"{key[0]}/{key[1]}"))
            if not had:
                return self.send_body(500, soap_fault(714, "NoSuchEntryInArray"))
            return self.send_body(200, soap_ok(action))
        log(dict(entry, result="401"))
        return self.send_body(500, soap_fault(401, "Invalid Action"))


def ssdp_loop(port, http_port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    while True:
        data, addr = s.recvfrom(2048)
        text = data.decode("utf-8", "replace")
        if not text.startswith("M-SEARCH"):
            continue
        st = re.search(r"^ST:\s*(.+?)\r?$", text, re.M | re.I)
        st = st.group(1).strip() if st else ""
        log({"call": "M-SEARCH", "from": f"{addr[0]}:{addr[1]}", "st": st})
        if "InternetGatewayDevice" not in st and "WANIPConnection" not in st:
            continue
        reply = ("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nEXT:\r\n"
                 f"LOCATION: http://127.0.0.1:{http_port}/rootDesc.xml\r\nSERVER: Mock/1.0 UPnP/1.0 MockIGD/1.0\r\n"
                 f"ST: {st}\r\nUSN: uuid:mock-igd::{st}\r\n\r\n")
        s.sendto(reply.encode("ascii"), addr)


def main():
    global args
    p = argparse.ArgumentParser()
    p.add_argument("--ssdp", type=int, default=19001)
    p.add_argument("--http", type=int, default=19002)
    p.add_argument("--external", default="203.0.113.7")
    p.add_argument("--log", required=True)
    p.add_argument("--refuse", action="store_true")
    p.add_argument("--permanent-only", action="store_true")
    p.add_argument("--chunked", action="store_true")
    args = p.parse_args()
    threading.Thread(target=ssdp_loop, args=(args.ssdp, args.http), daemon=True).start()
    log({"call": "START", "ssdp": args.ssdp, "http": args.http, "external": args.external})
    ThreadingHTTPServer(("127.0.0.1", args.http), Handler).serve_forever()


if __name__ == "__main__":
    main()
