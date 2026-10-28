$ErrorActionPreference = 'Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'RegistryStore.cs') -ReferencedAssemblies System.Runtime.Serialization,System.Xml
$fixture = 'Software\SteadyXR-RegistryStore-Tests\' + [guid]::NewGuid().ToString('N')
$root = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
$backupDir = Join-Path (Split-Path (Split-Path $PSScriptRoot)) ('build-windows\layer-store-tests-' + [guid]::NewGuid().ToString('N'))
$script:checks = 0
function Assert($condition, [string]$message) { if (-not $condition) { throw "ASSERT: $message" }; $script:checks++ }
function MustFail([scriptblock]$action, [string]$message) { $failed=$false; try { & $action } catch { $failed=$true }; Assert $failed $message }
function Value([string]$name, [uint32]$kind, [byte[]]$data) { $v=New-Object SteadyXR.LayerValue; $v.Name=$name; $v.Kind=$kind; $v.Data=$data; return $v }
try {
    $key = $root.CreateSubKey($fixture + '\Implicit')
    $key.SetValue('z-first.json', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
    $key.SetValue('a-second.json', [byte[]](0,255,12), [Microsoft.Win32.RegistryValueKind]::Binary)
    $key.Close()
    $store = New-Object SteadyXR.RegistryStore($fixture,$true)
    $before = $store.Read($true)
    foreach ($ownName in @('XR_APILAYER_STEADYXR_preview.json', 'XR_APILAYER_HORIZONLOCK_preview.json')) {
        $perUser = $store.Clone($before)
        $perUser[0].Values.Add((Value ('C:\fixture\' + $ownName) 4 ([BitConverter]::GetBytes([uint32]0))))
        MustFail { $null=$store.Apply($before,$perUser,$backupDir) } 'New per-user SteadyXR or legacy registration is rejected'
        Assert ($store.Same($before,$store.Read($true))) 'Rejected per-user registration leaves registry unchanged'
        $perUser[0].Values[$perUser[0].Values.Count-1].Data=[BitConverter]::GetBytes([uint32]1)
        $null=$store.Apply($before,$perUser,$backupDir)
        $enabled=$store.Clone($perUser)
        $enabled[0].Values[$enabled[0].Values.Count-1].Data=[BitConverter]::GetBytes([uint32]0)
        MustFail { $null=$store.Apply($perUser,$enabled,$backupDir) } 'Existing disabled per-user registration cannot be re-enabled by restore or toggle'
        $null=$store.Apply($perUser,$before,$backupDir)
    }
    $rawName=$store.Clone($before)
    $rawName[0].Values.Add((Value 'unrelated|malformed' 3 ([byte[]](1,3,5))))
    $null=$store.Apply($before,$rawName,$backupDir)
    $editedRaw=$store.Clone($rawName)
    $editedRaw[0].Values[0].Data=[BitConverter]::GetBytes([uint32]0)
    $null=$store.Apply($rawName,$editedRaw,$backupDir)
    Assert ($store.Same($editedRaw,$store.Read($true))) 'Malformed unrelated value names do not prevent valid edits'
    $null=$store.Apply($editedRaw,$before,$backupDir)
    Assert ($before.Count -eq 2) 'Read returns both ordered HKCU groups, including missing Explicit'
    Assert ($before[0].Values[0].Name -eq 'z-first.json') 'Raw enumeration order preserved'
    Assert ($before[0].Values[1].Kind -eq 3) 'Binary kind preserved'
    Assert ($before[0].Error -eq '' -and $before[1].Error -eq '') 'Both groups readable'
    Assert (-not $before[1].Exists -and $before[1].Values.Count -eq 0) 'Missing group preserved'
    Assert ($before[0].Hive -eq 'CurrentUser' -and $before[1].Hive -eq 'CurrentUser') 'Fixture mode reads only HKCU'
    $after=$store.Clone($before)
    $after[0].Values[0].Data[0]=0
    Assert ($before[0].Values[0].Data[0] -eq 1) 'Clone deep copies bytes'
    Assert (-not $store.Same($before,$after)) 'Same detects raw-byte changes'
    $backup=$store.Apply($before,$after,$backupDir)
    Assert ($store.Same($after,$store.Read($true))) 'Enable commits exact raw bytes'
    Assert ($store.Same($before,$store.ReadBackup($backup))) 'Durable JSON backup preserves exact raw snapshot'
    $undoBackup=$store.Apply($store.Read($true),$store.ReadBackup($backup),$backupDir)
    Assert ($store.Same($before,$store.Read($true))) 'Undo restores exact original order/data'
    Assert ($backup -ne $undoBackup) 'Backups have unique names'
    $securityKey=$root.OpenSubKey($fixture+'\Implicit')
    $securityBefore=$securityKey.GetAccessControl().GetSecurityDescriptorSddlForm([System.Security.AccessControl.AccessControlSections]'Access,Owner,Group'); $securityKey.Close()
    $reordered=$store.Clone($before); $reordered[0].Values.Reverse()
    $null=$store.Apply($before,$reordered,$backupDir)
    Assert ($store.Same($reordered,$store.Read($true))) 'Reorder changes exact native enumeration order'
    $null=$store.Apply($store.Read($true),$before,$backupDir)
    Assert ($store.Same($before,$store.Read($true))) 'Undo reorder restores native enumeration order'
    $securityKey=$root.OpenSubKey($fixture+'\Implicit')
    $securityAfter=$securityKey.GetAccessControl().GetSecurityDescriptorSddlForm([System.Security.AccessControl.AccessControlSections]'Access,Owner,Group'); $securityKey.Close()
    Assert ($securityBefore -eq $securityAfter) 'Reorder and undo preserve ACL owner and group'
    $child=$root.CreateSubKey($fixture+'\Implicit\vendor-child'); $child.SetValue('untouched','vendor'); $child.Close()
    MustFail { $null=$store.Apply($before,$reordered,$backupDir) } 'Reorder refuses unexpected child keys'
    Assert ($store.Same($before,$store.Read($true))) 'Unexpected child refusal leaves registrations unchanged'
    $child=$root.OpenSubKey($fixture+'\Implicit\vendor-child'); Assert ($child.GetValue('untouched') -eq 'vendor') 'Unexpected child data remains intact'; $child.Close()
    $key=$root.OpenSubKey($fixture+'\Implicit',$true); $key.DeleteSubKey('vendor-child'); $key.Close()
    $before=$store.Read($true); $after=$store.Clone($before)
    $after[0].Values[0].Data=[BitConverter]::GetBytes([uint32]1)
    $after[0].Values.RemoveAt(1)
    $after[1].Exists=$true
    $unicode='C:\layers\' + [char]0x00DC + [char]0x6F22 + '\manifest.json'
    $after[1].Values.Add((Value $unicode 4 ([BitConverter]::GetBytes([uint32]0))))
    $backup=$store.Apply($before,$after,$backupDir)
    Assert ($store.Same($after,$store.Read($true))) 'Disable/removal and new own-style Unicode registration commit across groups'
    Assert ($store.Read($true)[0].Values.Count -eq 1) 'One-value group reads correctly'
    $restored=$store.ReadBackup($backup)
    $null=$store.Apply($store.Read($true),$restored,$backupDir)
    $actual=$store.Read($true)
    Assert ($actual[1].Exists -and $actual[1].Values.Count -eq 0) 'Undo new key keeps key but clears registration'
    $restored[1].Exists=$true
    Assert ($store.Same($restored,$actual)) 'Undo preserves all data without deleting registry keys'

    $before=$store.Read($true); $after=$store.Clone($before)
    $after[0].Values.Clear()
    $after[0].Values.Add((Value '' 0 ([byte[]]@())))
    $after[0].Values.Add((Value 'string' 1 ([Text.Encoding]::Unicode.GetBytes("hello`0"))))
    $after[0].Values.Add((Value 'expand' 2 ([Text.Encoding]::Unicode.GetBytes("%TEMP%`0"))))
    $after[0].Values.Add((Value 'multi' 7 ([Text.Encoding]::Unicode.GetBytes("one`0two`0`0"))))
    $after[0].Values.Add((Value 'qword' 11 ([BitConverter]::GetBytes([long]123456789))))
    $after[0].Values.Add((Value 'unknown' 99 ([byte[]](1,3,5,7))))
    $after[0].Values.Add((Value 'malformed-dword' 4 ([byte[]](1,2,3))))
    $after[0].Values.Add((Value 'later-binary' 3 ([byte[]](0,255,3,0))))
    $backup=$store.Apply($before,$after,$backupDir)
    Assert ($store.Same($after,$store.Read($true))) 'All raw types, unknown type, malformed DWORD and later entries preserved'
    $before=$store.Read($true)
    $noop=$store.Apply($before,$store.Clone($before),(Join-Path $backupDir 'no-op-should-not-create'))
    Assert ($null -eq $noop -and -not (Test-Path (Join-Path $backupDir 'no-op-should-not-create'))) 'No-op does not write registry or backup'

    $after=$store.Clone($before); $after[0].Values.RemoveAt(0)
    $blockFile=Join-Path $backupDir 'not-a-directory'; Set-Content -LiteralPath $blockFile -Value 'blocked'
    MustFail { $null=$store.Apply($before,$after,$blockFile) } 'Backup failure is propagated'
    Assert ($store.Same($before,$store.Read($true))) 'Backup failure leaves registry untouched'
    $key=$root.OpenSubKey($fixture + '\Implicit',$true)
    $key.SetValue('external',42,[Microsoft.Win32.RegistryValueKind]::DWord); $key.Close()
    $external=$store.Read($true)
    MustFail { $null=$store.Apply($before,$after,$backupDir) } 'External change conflicts'
    Assert ($store.Same($external,$store.Read($true))) 'Conflict does not overwrite external edit'
    MustFail { $null=$store.Apply($external,$store.ReadBackup($backup),$blockFile) } 'Undo also requires successful backup'

    $before=$store.Read($true); $after=$store.Clone($before)
    $after[0].SubKey='Software\MustNeverWrite'
    MustFail { $null=$store.Apply($before,$after,$backupDir) } 'Identity changes rejected'
    $after=$store.Clone($before); $after[0].Values.Add($after[0].Values[0].Clone())
    MustFail { $null=$store.Apply($before,$after,$backupDir) } 'Duplicate names rejected'
    $after=$store.Clone($before); $after[0].Error='Read error'
    MustFail { $null=$store.Apply($before,$after,$backupDir) } 'Read-error snapshot rejected'
    MustFail { $null=New-Object SteadyXR.RegistryStore([SteadyXR.RegistryStore]::ProductionBasePath,$true) } 'Fixture mode refuses production root'

    # Deny the second group: preflight must reject before mutating the first group.
    $aclKey=$root.OpenSubKey($fixture+'\Explicit',[Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,[System.Security.AccessControl.RegistryRights]::ChangePermissions)
    $originalAcl=$aclKey.GetAccessControl()
    $changedAcl=$aclKey.GetAccessControl()
    $sid=[System.Security.Principal.WindowsIdentity]::GetCurrent().User
    $rule=New-Object System.Security.AccessControl.RegistryAccessRule($sid,[System.Security.AccessControl.RegistryRights]::SetValue,[System.Security.AccessControl.AccessControlType]::Deny)
    $changedAcl.AddAccessRule($rule)
    try {
        $aclKey.SetAccessControl($changedAcl)
        $before=$store.Read($true); $after=$store.Clone($before)
        $after[0].Values.RemoveAt(0)
        $after[1].Values.Add((Value 'write-denied.json' 4 ([BitConverter]::GetBytes([uint32]0))))
        MustFail { $null=$store.Apply($before,$after,$backupDir) } 'Second-group denied write fails preflight'
        Assert ($store.Same($before,$store.Read($true))) 'Preflight denial leaves first and second groups unchanged'
        # Unchanged denied group must never be opened for writing.
        $after=$store.Clone($before); $after[0].Values[0].Data=[byte[]](9,8,7)
        $null=$store.Apply($before,$after,$backupDir)
        Assert ($store.Same($after,$store.Read($true))) 'Unchanged groups require no write access'
    } finally { $changedAcl.RemoveAccessRuleSpecific($rule); $aclKey.SetAccessControl($changedAcl); $aclKey.Close() }

    $readAclKey=$root.OpenSubKey($fixture+'\Implicit',[Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,[System.Security.AccessControl.RegistryRights]::ChangePermissions)
    $readAcl=$readAclKey.GetAccessControl()
    $readDeny=New-Object System.Security.AccessControl.RegistryAccessRule($sid,[System.Security.AccessControl.RegistryRights]::QueryValues,[System.Security.AccessControl.AccessControlType]::Deny)
    try {
        $readAcl.AddAccessRule($readDeny); $readAclKey.SetAccessControl($readAcl)
        $partial=$store.Read($true)
        Assert (-not [string]::IsNullOrEmpty($partial[0].Error)) 'Denied read records a group error'
        Assert ($partial[1].Error -eq '' -and $partial[1].Exists) 'Denied group does not prevent later groups displaying'
        MustFail { $null=$store.Apply($partial,$store.Clone($partial),$backupDir) } 'Read-error groups cannot be blindly applied'
    } finally { $readAcl.RemoveAccessRuleSpecific($readDeny); $readAclKey.SetAccessControl($readAcl); $readAclKey.Close() }
    # A custom protected ACL exercises actual descriptor copying, rather than
    # merely obtaining the same inherited ACL on a new sibling key.
    $protectedPath=$fixture+'\Protected'
    $protectedKey=$root.CreateSubKey($protectedPath+'\Implicit')
    $protectedKey.SetValue('first.json',0,[Microsoft.Win32.RegistryValueKind]::DWord)
    $protectedKey.SetValue('second.json',1,[Microsoft.Win32.RegistryValueKind]::DWord)
    $customAcl=$protectedKey.GetAccessControl(); $customAcl.SetAccessRuleProtection($true,$false)
    $customRule=New-Object System.Security.AccessControl.RegistryAccessRule($sid,[System.Security.AccessControl.RegistryRights]::FullControl,[System.Security.AccessControl.AccessControlType]::Allow)
    $systemSid=New-Object System.Security.Principal.SecurityIdentifier('S-1-5-18')
    $systemRule=New-Object System.Security.AccessControl.RegistryAccessRule($systemSid,[System.Security.AccessControl.RegistryRights]::ReadKey,[System.Security.AccessControl.AccessControlType]::Allow)
    $customAcl.AddAccessRule($customRule); $customAcl.AddAccessRule($systemRule); $protectedKey.SetAccessControl($customAcl)
    $protectedAclBefore=$protectedKey.GetAccessControl().GetSecurityDescriptorSddlForm([System.Security.AccessControl.AccessControlSections]'Access,Owner,Group'); $protectedKey.Close()
    $protectedStore=New-Object SteadyXR.RegistryStore($protectedPath,$true)
    $protectedBefore=$protectedStore.Read($true); $protectedAfter=$protectedStore.Clone($protectedBefore); $protectedAfter[0].Values.Reverse()
    $null=$protectedStore.Apply($protectedBefore,$protectedAfter,$backupDir)
    Assert ($protectedStore.Same($protectedAfter,$protectedStore.Read($true))) 'Protected custom ACL permits actual reorder'
    $protectedKey=$root.OpenSubKey($protectedPath+'\Implicit'); $protectedAclAfter=$protectedKey.GetAccessControl().GetSecurityDescriptorSddlForm([System.Security.AccessControl.AccessControlSections]'Access,Owner,Group')
    Assert ($protectedKey.GetAccessControl().AreAccessRulesProtected) 'Reorder preserves ACL inheritance protection'
    # Windows RegCopyTree may clear AUTO_INHERITED bookkeeping (AI); compare
    # actual DACL bytes, owner/group and protection, which determine access.
    $descriptorBefore=New-Object System.Security.AccessControl.RawSecurityDescriptor($protectedAclBefore)
    $descriptorAfter=New-Object System.Security.AccessControl.RawSecurityDescriptor($protectedAclAfter)
    $daclBefore=New-Object byte[] $descriptorBefore.DiscretionaryAcl.BinaryLength; $descriptorBefore.DiscretionaryAcl.GetBinaryForm($daclBefore,0)
    $daclAfter=New-Object byte[] $descriptorAfter.DiscretionaryAcl.BinaryLength; $descriptorAfter.DiscretionaryAcl.GetBinaryForm($daclAfter,0)
    Assert ([Convert]::ToBase64String($daclBefore) -eq [Convert]::ToBase64String($daclAfter) -and $descriptorBefore.Owner.Value -eq $descriptorAfter.Owner.Value -and $descriptorBefore.Group.Value -eq $descriptorAfter.Group.Value) 'Reorder preserves custom protected DACL owner and group exactly'; $protectedKey.Close()
    $null=$protectedStore.Apply($protectedStore.Read($true),$protectedBefore,$backupDir)
    Assert ($protectedStore.Same($protectedBefore,$protectedStore.Read($true))) 'Protected custom ACL permits undo reorder'
    # A real concurrent ACL change after the first group starts writing forces
    # a partial-apply failure without a production test hook.
    Add-Type -TypeDefinition @"
using System;
using System.Threading;
using System.Security.AccessControl;
using System.Security.Principal;
using Microsoft.Win32;
public sealed class SteadyXRWriteFailureWatcher {
    public bool Triggered;
    public string Error;
    private Thread worker;
    public void Start(string fixture) {
        worker=new Thread(delegate() {
            try {
                using(RegistryKey root=RegistryKey.OpenBaseKey(RegistryHive.CurrentUser,RegistryView.Registry64)) {
                    DateTime deadline=DateTime.UtcNow.AddSeconds(10);
                    while(DateTime.UtcNow<deadline) {
                        using(RegistryKey first=root.OpenSubKey(fixture+@"\Implicit")) {
                            if(first!=null && first.GetValue("partial-write-marker")!=null) {
                                using(RegistryKey second=root.OpenSubKey(fixture+@"\Explicit",RegistryKeyPermissionCheck.ReadWriteSubTree,RegistryRights.ChangePermissions)) {
                                    RegistrySecurity acl=second.GetAccessControl();
                                    acl.AddAccessRule(new RegistryAccessRule(WindowsIdentity.GetCurrent().User,RegistryRights.SetValue,AccessControlType.Deny));
                                    second.SetAccessControl(acl); Triggered=true; return;
                                }
                            }
                        }
                        Thread.Sleep(1);
                    }
                    Error="Marker was not observed.";
                }
            } catch(Exception error) { Error=error.ToString(); }
        });
        worker.IsBackground=true; worker.Start();
    }
    public void Wait() { if(!worker.Join(12000)) throw new Exception("Watcher did not finish."); }
}
"@
    $before=$store.Read($true); $after=$store.Clone($before); $after[0].Values.Clear()
    $after[0].Values.Add((Value 'partial-write-marker' 4 ([BitConverter]::GetBytes([uint32]123))))
    $payload=New-Object byte[] 4096
    for($n=0; $n -lt 2000; $n++) { $after[0].Values.Add((Value ('fill-'+$n) 3 $payload)) }
    $after[1].Values.Add((Value 'second-group-write' 4 ([BitConverter]::GetBytes([uint32]0))))
    $watcher=New-Object SteadyXRWriteFailureWatcher
    $watcher.Start($fixture)
    $failureMessage=''
    try {
        try { $null=$store.Apply($before,$after,$backupDir) } catch { $failureMessage=$_.Exception.ToString() }
        $watcher.Wait()
        Assert ($watcher.Triggered -and -not $watcher.Error) 'Concurrent write denial triggered after first group mutation'
        Assert ($failureMessage -match 'Original registrations were restored and verified') 'Partial apply failure reports verified restoration'
        Assert ($store.Same($before,$store.Read($true))) 'Partial first-group writes fully restored after later write denial'
    } finally {
        $watcher.Wait()
        $aclKey=$root.OpenSubKey($fixture+'\Explicit',[Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,[System.Security.AccessControl.RegistryRights]::ChangePermissions)
        $acl=$aclKey.GetAccessControl(); $acl.RemoveAccessRuleSpecific($rule); $aclKey.SetAccessControl($acl); $aclKey.Close()
    }
    Add-Type -TypeDefinition @"
using System;
using System.Threading;
using Microsoft.Win32;
public sealed class SteadyXRExternalRegistrationWatcher {
    public bool Triggered;
    public string Error;
    private Thread worker;
    public void Start(string fixture) {
        worker=new Thread(delegate() {
            try {
                using(RegistryKey root=RegistryKey.OpenBaseKey(RegistryHive.CurrentUser,RegistryView.Registry64)) {
                    DateTime deadline=DateTime.UtcNow.AddSeconds(10);
                    while(DateTime.UtcNow<deadline) {
                        using(RegistryKey second=root.OpenSubKey(fixture+@"\Explicit")) {
                            if(second!=null && second.GetValue("second-write-marker")!=null) {
                                using(RegistryKey first=root.OpenSubKey(fixture+@"\Implicit",true)) {
                                    first.SetValue("external-installer.json",77,RegistryValueKind.DWord);
                                    Triggered=true; return;
                                }
                            }
                        }
                        Thread.Sleep(1);
                    }
                    Error="Marker was not observed.";
                }
            } catch(Exception error) { Error=error.ToString(); }
        });
        worker.IsBackground=true; worker.Start();
    }
    public void Wait() { if(!worker.Join(12000)) throw new Exception("External watcher did not finish."); }
}
"@
    $before=$store.Read($true); $after=$store.Clone($before)
    $after[0].Values.Add((Value 'own-first-marker' 4 ([BitConverter]::GetBytes([uint32]1))))
    $after[1].Values.Add((Value 'second-write-marker' 4 ([BitConverter]::GetBytes([uint32]1))))
    for($n=0; $n -lt 2000; $n++) { $after[1].Values.Add((Value ('second-fill-'+$n) 3 $payload)) }
    $externalWatcher=New-Object SteadyXRExternalRegistrationWatcher
    $externalWatcher.Start($fixture)
    $externalFailure=''
    try {
        try { $null=$store.Apply($before,$after,$backupDir) } catch { $externalFailure=$_.Exception.ToString() }
        $externalWatcher.Wait()
        Assert ($externalWatcher.Triggered -and -not $externalWatcher.Error) 'External installer adds value after first group completed'
        $current=$store.Read($true)
        $externalValue=@($current[0].Values | Where-Object Name -eq 'external-installer.json')
        Assert ($externalValue.Count -eq 1 -and [BitConverter]::ToUInt32($externalValue[0].Data,0) -eq 77) 'Rollback preserves unexpected external installer registration'
        Assert ($externalFailure -match 'Restoration could not be verified|Restoration incomplete') 'Conflicted rollback clearly reports incomplete restoration'
        Assert ($store.Same($before.GetRange(1,1),$current.GetRange(1,1))) 'Unconflicted second group still restores exactly'
    } finally { $externalWatcher.Wait() }
    $before=$store.Read($true); $after=$store.Clone($before); $after[0].Values.Clear()
    $null=$store.Apply($before,$after,$backupDir)
    Assert ($store.Read($true)[0].Exists -and $store.Read($true)[0].Values.Count -eq 0) 'Zero-value existing group reads correctly'
    $read32=$store.Read($false)
    Assert ($read32.Count -eq 2 -and -not $read32[0].Is64Bit) '32-bit view identities supported'
} finally {
    # Only this unique HKCU fixture subtree is deleted; no production registration is touched.
    if (-not $fixture.StartsWith('Software\SteadyXR-RegistryStore-Tests\')) { throw 'Unsafe fixture cleanup path' }
    $root.Close(); & reg.exe delete ("HKCU\"+$fixture) /f | Out-Null; if ($LASTEXITCODE -ne 0) { throw "Fixture cleanup failed: $fixture" }
}
Write-Host "PASS $script:checks RegistryStore checks; private HKCU fixture only. Backups: $backupDir"
