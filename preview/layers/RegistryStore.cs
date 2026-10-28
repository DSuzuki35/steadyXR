using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Text;

namespace SteadyXR
{
    [DataContract]
    public sealed class LayerValue
    {
        [DataMember(Order=0)] public string Name { get; set; }
        [DataMember(Order=1)] public uint Kind { get; set; }
        [DataMember(Order=2)] public byte[] Data { get; set; }
        public LayerValue() { Name = ""; Data = new byte[0]; }
        public LayerValue Clone() { return new LayerValue { Name=Name, Kind=Kind, Data=Data == null ? null : (byte[])Data.Clone() }; }
    }

    [DataContract]
    public sealed class LayerGroup
    {
        [DataMember(Order=0)] public string Hive { get; set; }
        [DataMember(Order=1)] public bool Is64Bit { get; set; }
        [DataMember(Order=2)] public string SubKey { get; set; }
        [DataMember(Order=3)] public bool Exists { get; set; }
        [DataMember(Order=4)] public List<LayerValue> Values { get; set; }
        [DataMember(Order=5)] public string Error { get; set; }
        public LayerGroup() { Values = new List<LayerValue>(); Error = ""; }
        public LayerGroup Clone()
        {
            LayerGroup result = new LayerGroup { Hive=Hive, Is64Bit=Is64Bit, SubKey=SubKey, Exists=Exists, Error=Error };
            if (Values == null) result.Values = null;
            else foreach (LayerValue value in Values) result.Values.Add(value == null ? null : value.Clone());
            return result;
        }
    }

    // All reads and writes use raw native bytes. No type conversion or sorting occurs.
    public sealed class RegistryStore
    {
        public const string ProductionBasePath = @"SOFTWARE\Khronos\OpenXR\1\ApiLayers";
        private readonly string basePath;
        private readonly bool userOnly;
        private const uint ReadAccess = 0x20019;
        private const int NotFound = 2, MoreData = 234, NoMoreItems = 259;
        public RegistryStore() : this(ProductionBasePath, false) { }
        public RegistryStore(string basePath, bool userOnly)
        {
            if (String.IsNullOrWhiteSpace(basePath) || basePath.IndexOf('\0') >= 0)
                throw new ArgumentException("A registry base path is required.");
            this.basePath = basePath.Trim('\\');
            this.userOnly = userOnly;
            if (userOnly && (String.Equals(this.basePath, ProductionBasePath, StringComparison.OrdinalIgnoreCase) ||
                this.basePath.StartsWith(ProductionBasePath + "\\", StringComparison.OrdinalIgnoreCase)))
                throw new ArgumentException("Fixture mode requires a private registry base path.");
        }

        public List<LayerGroup> Clone(List<LayerGroup> groups)
        {
            if (groups == null) throw new ArgumentNullException("groups");
            List<LayerGroup> result = new List<LayerGroup>();
            foreach (LayerGroup group in groups) result.Add(group == null ? null : group.Clone());
            return result;
        }
        private static bool SameGroup(LayerGroup a, LayerGroup b)
        {
            if (a == null || b == null) return a == b;
            if (a.Hive != b.Hive || a.Is64Bit != b.Is64Bit || a.SubKey != b.SubKey || a.Exists != b.Exists ||
                (a.Error ?? "") != (b.Error ?? "") || a.Values == null || b.Values == null || a.Values.Count != b.Values.Count) return false;
            for (int i=0; i<a.Values.Count; ++i)
            {
                LayerValue x=a.Values[i], y=b.Values[i];
                if (x == null || y == null || x.Name != y.Name || x.Kind != y.Kind || x.Data == null || y.Data == null || x.Data.Length != y.Data.Length) return false;
                for (int j=0; j<x.Data.Length; ++j) if (x.Data[j] != y.Data[j]) return false;
            }
            return true;
        }
        public bool Same(List<LayerGroup> a, List<LayerGroup> b)
        {
            if (a == null || b == null || a.Count != b.Count) return false;
            for (int i=0; i<a.Count; ++i) if (!SameGroup(a[i],b[i])) return false;
            return true;
        }
        private List<LayerGroup> Identities(bool is64Bit)
        {
            List<LayerGroup> result = new List<LayerGroup>();
            foreach (string type in new string[] { "Implicit", "Explicit" })
            {
                if (!userOnly) result.Add(new LayerGroup { Hive="LocalMachine", Is64Bit=is64Bit, SubKey=basePath+"\\"+type });
                result.Add(new LayerGroup { Hive="CurrentUser", Is64Bit=is64Bit, SubKey=basePath+"\\"+type });
            }
            return result;
        }
        public List<LayerGroup> Read(bool is64Bit) { return ReadAll(is64Bit); }
        private List<LayerGroup> ReadAll(bool is64Bit)
        {
            List<LayerGroup> groups = Identities(is64Bit);
            foreach (LayerGroup group in groups) ReadGroup(group);
            return groups;
        }
        private static IntPtr Hive(LayerGroup group) { return new IntPtr(unchecked((int)(group.Hive == "CurrentUser" ? 0x80000001u : 0x80000002u))); }
        private static uint Access(LayerGroup group, uint access) { return access | (group.Is64Bit ? 0x100u : 0x200u); }
        private static int Open(LayerGroup group, uint access, out IntPtr handle)
        {
            return RegOpenKeyEx(Hive(group), group.SubKey, 0, Access(group,access), out handle);
        }
        private static void Check(int code, string action)
        {
            if (code != 0) throw new Win32Exception(code, action + ": " + new Win32Exception(code).Message);
        }
        private static void ReadGroup(LayerGroup group)
        {
            IntPtr handle=IntPtr.Zero;
            try
            {
                int code=Open(group,ReadAccess,out handle);
                if (code == NotFound) return;
                Check(code,"Read " + group.Hive + "\\" + group.SubKey);
                group.Exists=true;
                uint count, maxName, maxData; long stamp;
                Query(handle,out count,out maxName,out maxData,out stamp);
                for (uint index=0; index<count; ++index)
                {
                    try
                    {
                        uint nameSize=Math.Max(256,maxName+1), dataSize=Math.Max(256,maxData);
                        for (int attempt=0; ; ++attempt)
                        {
                            if (nameSize>32768 || dataSize>67108864) throw new IOException("Registry value exceeds safe read limits at index " + index + ".");
                            StringBuilder name=new StringBuilder((int)nameSize);
                            byte[] data=new byte[(int)dataSize];
                            uint chars=nameSize, bytes=dataSize, kind;
                            code=RegEnumValue(handle,index,name,ref chars,IntPtr.Zero,out kind,data,ref bytes);
                            if (code == MoreData && attempt<8) { nameSize=Math.Max(nameSize*2,chars+1); dataSize=Math.Max(dataSize*2,bytes); continue; }
                            Check(code,"Read value index " + index);
                            Array.Resize(ref data,(int)bytes);
                            group.Values.Add(new LayerValue { Name=name.ToString(), Kind=kind, Data=data });
                            break;
                        }
                    }
                    catch (Exception error) { group.Error += error.Message + " "; }
                }
                uint endCount,endName,endData; long endStamp;
                Query(handle,out endCount,out endName,out endData,out endStamp);
                if (endCount != count || endStamp != stamp) group.Error += "Registry changed during read; refresh required.";
            }
            catch (Exception error) { group.Error += error.Message; }
            finally { if (handle != IntPtr.Zero) RegCloseKey(handle); }
        }
        private static void Query(IntPtr handle,out uint count,out uint maxName,out uint maxData,out long stamp)
        {
            uint subKeys,maxSubKey,maxClass,security;
            Check(RegQueryInfoKey(handle,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero,out subKeys,out maxSubKey,out maxClass,out count,out maxName,out maxData,out security,out stamp),"Inspect registry key");
        }
        private void Validate(List<LayerGroup> groups)
        {
            if (groups == null || groups.Count == 0 || groups[0] == null) throw new ArgumentException("Registry snapshot is missing.");
            List<LayerGroup> identities=Identities(groups[0].Is64Bit);
            if (identities.Count != groups.Count) throw new ArgumentException("Registry snapshot group count is invalid.");
            for (int i=0; i<groups.Count; ++i)
            {
                LayerGroup group=groups[i], identity=identities[i];
                if (group == null || group.Hive != identity.Hive || group.Is64Bit != identity.Is64Bit || group.SubKey != identity.SubKey)
                    throw new ArgumentException("Registry snapshot identity changed.");
                if (!String.IsNullOrEmpty(group.Error)) throw new InvalidOperationException("Registry snapshot has read errors: " + group.Error);
                if (group.Values == null || (!group.Exists && group.Values.Count != 0)) throw new ArgumentException("Invalid registry group values.");
                HashSet<string> names=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (LayerValue value in group.Values)
                    if (value == null || value.Name == null || value.Name.Length>16383 || value.Name.IndexOf('\0')>=0 || value.Data == null || !names.Add(value.Name))
                        throw new ArgumentException("Invalid or duplicate registry value.");
            }
        }
        private static DataContractJsonSerializer Serializer() { return new DataContractJsonSerializer(typeof(List<LayerGroup>)); }
        private string Backup(List<LayerGroup> before, string directory)
        {
            if (String.IsNullOrWhiteSpace(directory)) throw new ArgumentException("Backup directory is required.");
            Directory.CreateDirectory(directory);
            string path=Path.Combine(Path.GetFullPath(directory),"SteadyXR-layers-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fffffff") + "-" + Guid.NewGuid().ToString("N") + ".json");
            using (FileStream stream=new FileStream(path,FileMode.CreateNew,FileAccess.Write,FileShare.None,4096,FileOptions.WriteThrough))
            {
                Serializer().WriteObject(stream,before);
                stream.Flush(true);
            }
            if (!Same(before,ReadBackup(path))) throw new IOException("Backup verification failed; registry is unchanged.");
            return path;
        }
        public List<LayerGroup> ReadBackup(string path)
        {
            List<LayerGroup> result;
            using (FileStream stream=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read)) result=(List<LayerGroup>)Serializer().ReadObject(stream);
            Validate(result);
            return result;
        }
        public string Apply(List<LayerGroup> before, List<LayerGroup> after, string backupDirectory)
        {
            before=Clone(before); after=Clone(after);
            Validate(before); Validate(after);
            if (before[0].Is64Bit != after[0].Is64Bit) throw new ArgumentException("Snapshot architecture changed.");
            // Legacy HKCU entries may be inspected, disabled or removed, but neither
            // toggling nor restoring a backup may install/enable SteadyXR per-user.
            for (int i=0; i<after.Count; ++i)
            {
                if (after[i].Hive != "CurrentUser") continue;
                foreach (LayerValue value in after[i].Values)
                {
                    // Registry value names may contain characters forbidden in filesystem
                    // paths. Inspect the basename lexically; keep unrelated raw names valid.
                    string name=value.Name.Replace('/','\\');
                    string filename=name.Substring(name.LastIndexOf('\\')+1);
                    bool own=String.Equals(filename,"XR_APILAYER_STEADYXR_preview.json",StringComparison.OrdinalIgnoreCase) ||
                             String.Equals(filename,"XR_APILAYER_HORIZONLOCK_preview.json",StringComparison.OrdinalIgnoreCase);
                    if (own && value.Kind==4 && value.Data.Length==4 && BitConverter.ToUInt32(value.Data,0)==0 &&
                        !SameValue(FindValue(before[i],value.Name),value))
                        throw new InvalidOperationException("SteadyXR must be installed system-wide. Use Install / repair SteadyXR and approve the administrator prompt.");
                }
            }
            using (System.Threading.Mutex mutex=new System.Threading.Mutex(false,@"Local\SteadyXR-LayerRegistryWrites"))
            {
                bool acquired=false;
                try
                {
                    try { acquired=mutex.WaitOne(10000); }
                    catch (System.Threading.AbandonedMutexException) { acquired=true; }
                    if (!acquired) throw new IOException("Another SteadyXR editor is applying changes; try again.");
                    if (!Same(before,Read(before[0].Is64Bit))) throw new InvalidOperationException("Registry changed externally or could not be read; refresh before applying.");
                    if (Same(before,after)) return null;
                    List<int> changed=new List<int>();
                    List<LayerGroup> expected=Clone(after);
                    for (int i=0; i<before.Count; ++i)
                    {
                        if (SameGroup(before[i],after[i])) continue;
                        changed.Add(i);
                        if (before[i].Exists && !after[i].Exists) expected[i].Exists=true;
                        Preflight(before[i]);
                        if (NeedsReorder(before[i],after[i])) PreflightReorder(before[i]);
                    }
                    string backup=Backup(before,backupDirectory);
                    if (!Same(before,Read(before[0].Is64Bit))) throw new InvalidOperationException("Registry changed during backup/access checks; refresh before applying. Backup: " + backup);
                    List<int> touched=new List<int>();
                    List<WriteJournal> journals=new List<WriteJournal>();
                    foreach (LayerGroup group in before) journals.Add(new WriteJournal(group));
                    try
                    {
                        foreach (int i in changed)
                        {
                            LayerGroup latest=Identities(before[i].Is64Bit)[i];
                            ReadGroup(latest);
                            if (!SameGroup(before[i],latest)) throw new InvalidOperationException("Registry changed externally during Apply.");
                            touched.Add(i);
                            WriteGroup(before[i],expected[i],journals[i]);
                        }
                        List<LayerGroup> written=Read(before[0].Is64Bit);
                        if (!Same(expected,written)) throw new IOException("Registry did not preserve the requested order and raw values.");
                        return backup;
                    }
                    catch (Exception applyError)
                    {
                        string recoveryErrors="";
                        for (int t=touched.Count-1; t>=0; --t)
                        {
                            int i=touched[t];
                            try
                            {
                                LayerGroup latest=Identities(before[i].Is64Bit)[i];
                                ReadGroup(latest);
                                if (!String.IsNullOrEmpty(latest.Error)) throw new IOException(latest.Error);
                                LayerGroup restore=before[i].Clone();
                                if (latest.Exists && !restore.Exists) restore.Exists=true;
                                if (!SameContents(latest,journals[i].Expected))
                                    throw new IOException("External changes detected in " + latest.Hive + "\\" + latest.SubKey + "; this group was left untouched during rollback.");
                                if (!SameGroup(latest,restore)) WriteGroup(journals[i].Expected,restore,new WriteJournal(journals[i].Expected));
                            }
                            catch (Exception recoveryError) { recoveryErrors += recoveryError.Message + " "; }
                        }
                        List<LayerGroup> restored=Read(before[0].Is64Bit), restoreExpected=Clone(before);
                        foreach (int i in touched) if (!restoreExpected[i].Exists && restored[i].Exists) restoreExpected[i].Exists=true;
                        bool verified=Same(restoreExpected,restored);
                        throw new IOException("Apply failed: " + applyError.Message + (verified ? " Original registrations were restored and verified." : " Restoration could not be verified; use Restore backup and inspect registry state. " + recoveryErrors) + " Backup: " + backup,applyError);
                    }
                }
                finally { if (acquired) mutex.ReleaseMutex(); }
            }
        }
        // The journal advances only after a native operation succeeds. Rollback
        // may restore this known state; it must never treat a fresh external
        // snapshot as a list of values the editor owns and is allowed to delete.
        private sealed class WriteJournal
        {
            public LayerGroup Expected;
            public WriteJournal(LayerGroup before) { Expected=before.Clone(); }
        }
        private static LayerValue FindValue(LayerGroup group,string name)
        {
            foreach (LayerValue value in group.Values)
                if (String.Equals(value.Name,name,StringComparison.OrdinalIgnoreCase)) return value;
            return null;
        }
        private static bool SameValue(LayerValue a,LayerValue b)
        {
            if (a==null || b==null) return a==b;
            if (a.Name!=b.Name || a.Kind!=b.Kind || a.Data.Length!=b.Data.Length) return false;
            for (int i=0;i<a.Data.Length;++i) if(a.Data[i]!=b.Data[i]) return false;
            return true;
        }
        private static bool SameContents(LayerGroup a,LayerGroup b)
        {
            if (a.Hive!=b.Hive || a.SubKey!=b.SubKey || a.Is64Bit!=b.Is64Bit || a.Exists!=b.Exists ||
                !String.IsNullOrEmpty(a.Error) || !String.IsNullOrEmpty(b.Error) || a.Values.Count!=b.Values.Count) return false;
            foreach (LayerValue value in a.Values) if (!SameValue(value,FindValue(b,value.Name))) return false;
            return true;
        }
        private static void CheckOwnedValue(IntPtr handle,string name,LayerValue expected)
        {
            uint kind,size=0;
            int code=RegQueryValueEx(handle,name,IntPtr.Zero,out kind,null,ref size);
            if (code==NotFound)
            {
                if (expected==null) return;
                throw new IOException("Registration changed externally before writing: " + name);
            }
            Check(code,"Check registration before writing " + name);
            for (int attempt=0; ; ++attempt)
            {
                if (size>67108864) throw new IOException("Registry value exceeds safe read limits.");
                byte[] data=new byte[(int)size]; uint bytes=size;
                code=RegQueryValueEx(handle,name,IntPtr.Zero,out kind,data,ref bytes);
                if(code==MoreData && attempt<8) { size=bytes; continue; }
                Check(code,"Recheck registration before writing " + name);
                Array.Resize(ref data,(int)bytes);
                LayerValue actual=new LayerValue { Name=name, Kind=kind, Data=data };
                if (!SameValue(actual,expected)) throw new IOException("Registration changed externally before writing: " + name);
                return;
            }
        }
        private static bool NeedsReorder(LayerGroup before, LayerGroup after)
        {
            if (!before.Exists || after.Values.Count<2) return false;
            List<string> natural=new List<string>();
            HashSet<string> desiredNames=new HashSet<string>(StringComparer.OrdinalIgnoreCase), oldNames=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (LayerValue value in after.Values) desiredNames.Add(value.Name);
            foreach (LayerValue value in before.Values) { oldNames.Add(value.Name); if (desiredNames.Contains(value.Name)) natural.Add(value.Name); }
            foreach (LayerValue value in after.Values) if (!oldNames.Contains(value.Name)) natural.Add(value.Name);
            for (int i=0;i<natural.Count;++i) if (natural[i]!=after.Values[i].Name) return true;
            return false;
        }
        private static void PreflightReorder(LayerGroup group)
        {
            IntPtr handle=IntPtr.Zero;
            try { Check(Open(group,ReadAccess|0x10000,out handle),"Check reorder access"); RequireFlat(handle); }
            finally { if(handle!=IntPtr.Zero) RegCloseKey(handle); }
            LayerGroup parent=group.Clone(); parent.SubKey=parent.SubKey.Substring(0,parent.SubKey.LastIndexOf('\\'));
            handle=IntPtr.Zero;
            try { Check(Open(parent,0xF003F,out handle),"Check reorder parent access"); }
            finally { if(handle!=IntPtr.Zero) RegCloseKey(handle); }
        }
        private static void Preflight(LayerGroup group)
        {
            LayerGroup probe=group.Clone();
            for (;;)
            {
                IntPtr handle=IntPtr.Zero;
                int code=Open(probe,probe.SubKey == group.SubKey ? ReadAccess|2u : ReadAccess|4u,out handle);
                if (handle != IntPtr.Zero) RegCloseKey(handle);
                if (code != NotFound) { Check(code,"Check write access " + group.Hive + "\\" + group.SubKey); return; }
                int slash=probe.SubKey.LastIndexOf('\\');
                if (slash < 0) probe.SubKey="";
                else probe.SubKey=probe.SubKey.Substring(0,slash);
            }
        }
        private static void WriteGroup(LayerGroup before, LayerGroup after, WriteJournal journal)
        {
            IntPtr handle=IntPtr.Zero;
            try
            {
                int code=Open(before,ReadAccess|2u,out handle);
                if (code == NotFound && after.Exists)
                {
                    uint disposition;
                    code=RegCreateKeyEx(Hive(after),after.SubKey,0,null,0,Access(after,ReadAccess|2u),IntPtr.Zero,out handle,out disposition);
                }
                if (code == NotFound && !after.Exists) return;
                Check(code,"Write " + after.Hive + "\\" + after.SubKey);
                journal.Expected.Exists=true;
                // Preserve unchanged values in place. Check each value immediately
                // before our own set/delete; unrelated registrations are never swept.
                foreach (LayerValue value in before.Values)
                {
                    if (FindValue(after,value.Name)!=null) continue;
                    CheckOwnedValue(handle,value.Name,value);
                    Check(RegDeleteValue(handle,value.Name),"Remove registration " + value.Name);
                    journal.Expected.Values.Remove(FindValue(journal.Expected,value.Name));
                }
                foreach (LayerValue value in after.Values)
                {
                    LayerValue prior=FindValue(journal.Expected,value.Name);
                    if (SameValue(prior,value)) continue;
                    CheckOwnedValue(handle,value.Name,prior);
                    Check(RegSetValueEx(handle,value.Name,0,value.Kind,value.Data,(uint)value.Data.Length),"Write registration " + value.Name);
                    if(prior==null) journal.Expected.Values.Add(value.Clone());
                    else journal.Expected.Values[journal.Expected.Values.IndexOf(prior)]=value.Clone();
                }
            }
            finally { if (handle != IntPtr.Zero) RegCloseKey(handle); }
            LayerGroup written=new LayerGroup { Hive=after.Hive, Is64Bit=after.Is64Bit, SubKey=after.SubKey };
            ReadGroup(written);
            if (!String.IsNullOrEmpty(written.Error)) throw new IOException("Registry readback failed: " + written.Error);
            if (!SameContents(written,after)) throw new IOException("Registry contents changed externally during writing; refusing to overwrite them.");
            if (!SameGroup(written,after)) ReplaceForOrder(after);
            journal.Expected=after.Clone();
        }
        private static void RequireFlat(IntPtr handle)
        {
            uint subKeys,maxSubKey,maxClass,count,maxName,maxData,security; long stamp;
            Check(RegQueryInfoKey(handle,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero,out subKeys,out maxSubKey,out maxClass,out count,out maxName,out maxData,out security,out stamp),"Inspect reorder key");
            if (subKeys != 0) throw new IOException("Cannot reorder a registry key containing unexpected subkeys; no subkeys will be replaced or deleted.");
        }
        // Some Windows builds preserve deleted value slots. A fresh sibling key is
        // needed to obtain a new enumeration order. Unexpected child keys are refused; RegCopyTree preserves
        // security; the original key is retained until the replacement is verified.
        private static void ReplaceForOrder(LayerGroup desired)
        {
            int split=desired.SubKey.LastIndexOf('\\');
            string parentPath=desired.SubKey.Substring(0,split), leaf=desired.SubKey.Substring(split+1);
            string suffix=Guid.NewGuid().ToString("N"), tempLeaf="SteadyXR-staging-"+suffix, oldLeaf="SteadyXR-recovery-"+suffix;
            LayerGroup staging=desired.Clone(); staging.SubKey=parentPath+"\\"+tempLeaf;
            IntPtr parent=IntPtr.Zero, source=IntPtr.Zero, target=IntPtr.Zero;
            bool oldRenamed=false, newRenamed=false, created=false, activatedOnce=false;
            try
            {
                Check(RegOpenKeyEx(Hive(desired),parentPath,0,Access(desired,0xF003F),out parent),"Open parent for reorder");
                Check(Open(desired,ReadAccess|0x10000,out source),"Open original key for reorder");
                RequireFlat(source);
                uint disposition;
                Check(RegCreateKeyEx(parent,tempLeaf,0,null,0,Access(desired,0xF003F),IntPtr.Zero,out target,out disposition),"Create reorder staging key");
                created=true;
                foreach (LayerValue value in desired.Values) Check(RegSetValueEx(target,value.Name,0,value.Kind,value.Data,(uint)value.Data.Length),"Stage ordered registration");
                Check(RegCopyTree(source,null,target),"Preserve registry key security");
                RegCloseKey(target); target=IntPtr.Zero;
                LayerGroup staged=new LayerGroup { Hive=desired.Hive, Is64Bit=desired.Is64Bit, SubKey=staging.SubKey };
                ReadGroup(staged); staged.SubKey=desired.SubKey;
                if (!SameGroup(staged,desired)) throw new IOException("Staged reorder did not preserve raw registry values.");
                RegCloseKey(source); source=IntPtr.Zero;
                LayerGroup originalNow=new LayerGroup { Hive=desired.Hive, Is64Bit=desired.Is64Bit, SubKey=desired.SubKey };
                ReadGroup(originalNow);
                if (!SameContents(originalNow,desired)) throw new IOException("Registry changed externally before reorder activation.");
                Check(RegRenameKey(parent,leaf,oldLeaf),"Retain original key for reorder recovery"); oldRenamed=true;
                Check(RegRenameKey(parent,tempLeaf,leaf),"Activate reordered registrations"); newRenamed=true; activatedOnce=true;
                LayerGroup activated=new LayerGroup { Hive=desired.Hive, Is64Bit=desired.Is64Bit, SubKey=desired.SubKey };
                ReadGroup(activated);
                if (!SameGroup(activated,desired)) throw new IOException("Activated reorder verification failed.");
                LayerGroup retained=new LayerGroup { Hive=desired.Hive, Is64Bit=desired.Is64Bit, SubKey=parentPath+"\\"+oldLeaf };
                ReadGroup(retained); retained.SubKey=desired.SubKey;
                if (!SameContents(retained,desired)) throw new IOException("Registry changed externally in the retained original key; preserving it.");
                Check(RegDeleteKeyEx(parent,oldLeaf,desired.Is64Bit ? 0x100u : 0x200u,0),"Remove verified reorder recovery key");
                oldRenamed=false;
            }
            catch (Exception reorderError)
            {
                if (newRenamed)
                {
                    // Move the replacement out of the way before restoring the retained original.
                    if (RegRenameKey(parent,leaf,tempLeaf)==0) newRenamed=false;
                }
                if (oldRenamed && !newRenamed)
                {
                    if (RegRenameKey(parent,oldLeaf,leaf)==0) oldRenamed=false;
                }
                throw new IOException("Reorder failed: " + reorderError.Message + (oldRenamed ? " Original key recovery path: " + desired.Hive + "\\" + parentPath + "\\" + oldLeaf : " Original key name restored.") + (activatedOnce && !newRenamed ? " Retained replacement recovery path: " + desired.Hive + "\\" + staging.SubKey : ""),reorderError);
            }
            finally
            {
                if (source!=IntPtr.Zero) RegCloseKey(source);
                if (target!=IntPtr.Zero) RegCloseKey(target);
                if (parent!=IntPtr.Zero)
                {
                    // A once-active replacement may contain an installer's new values; retain it on failure.
                    if (created && !newRenamed && !activatedOnce) { RegDeleteKeyEx(parent,tempLeaf,desired.Is64Bit ? 0x100u : 0x200u,0); }
                    RegCloseKey(parent);
                }
            }
        }
        [DllImport("advapi32.dll",CharSet=CharSet.Unicode)] private static extern int RegCopyTree(IntPtr source,string subKey,IntPtr destination);
        [DllImport("advapi32.dll",CharSet=CharSet.Unicode,ExactSpelling=true)] private static extern int RegRenameKey(IntPtr key,string subKey,string newName);
        [DllImport("advapi32.dll",CharSet=CharSet.Unicode)] private static extern int RegDeleteKeyEx(IntPtr key,string subKey,uint view,uint reserved);
        [DllImport("advapi32.dll",CharSet=CharSet.Unicode)] private static extern int RegOpenKeyEx(IntPtr key,string subKey,uint options,uint access,out IntPtr result);
        [DllImport("advapi32.dll",CharSet=CharSet.Unicode)] private static extern int RegCreateKeyEx(IntPtr key,string subKey,uint reserved,string keyClass,uint options,uint access,IntPtr security,out IntPtr result,out uint disposition);
        [DllImport("advapi32.dll",CharSet=CharSet.Unicode)] private static extern int RegEnumValue(IntPtr key,uint index,StringBuilder name,ref uint nameLength,IntPtr reserved,out uint kind,byte[] data,ref uint dataLength);
        [DllImport("advapi32.dll",CharSet=CharSet.Unicode)] private static extern int RegQueryInfoKey(IntPtr key,IntPtr keyClass,IntPtr classLength,IntPtr reserved,out uint subKeys,out uint maxSubKey,out uint maxClass,out uint values,out uint maxName,out uint maxData,out uint security,out long lastWrite);
        [DllImport("advapi32.dll",CharSet=CharSet.Unicode)] private static extern int RegQueryValueEx(IntPtr key,string name,IntPtr reserved,out uint kind,byte[] data,ref uint length);
        [DllImport("advapi32.dll",CharSet=CharSet.Unicode)] private static extern int RegSetValueEx(IntPtr key,string name,uint reserved,uint kind,byte[] data,uint length);
        [DllImport("advapi32.dll",CharSet=CharSet.Unicode)] private static extern int RegDeleteValue(IntPtr key,string name);
        [DllImport("advapi32.dll")] private static extern int RegCloseKey(IntPtr key);
    }
}
