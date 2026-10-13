# Print the PC-side network adapter of the TB330FU USB gadget (host MAC 02-00-00-00-00-02): ifIndex, name, status, IPv4 addresses, IPv6 link-local
$a = Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object { $_.MacAddress -eq '02-00-00-00-00-02' } | Select-Object -First 1
if ($a) {
    $ips = (Get-NetIPAddress -InterfaceIndex $a.ifIndex -ErrorAction SilentlyContinue | ForEach-Object { $_.IPAddress }) -join ','
    "{0}`t{1}`t{2}`t{3}" -f $a.ifIndex, $a.Name, $a.Status, $ips
}
