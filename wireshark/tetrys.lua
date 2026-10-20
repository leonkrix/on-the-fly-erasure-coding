tetrys_protocol = Proto("Tetrys", "Tetrys Protocol")

packet_type = {
    [0] = "Source Packet",
    [1] = "Coded Packet",
    [2] = "Window Update Packet",
    [10] = "END Message",
    [11] = "ACK END Message",
    [12] = "RDY Message",
}

MessageType = ProtoField.uint8("tetrys.packet_type", "PacketType", base.DEC, packet_type)
MessageId = ProtoField.uint16("tetrys.id", "ID", base.DEC)
SmallestGenerationUsed = ProtoField.uint64("tetrys.smallest_generation_used", "SmallestGenerationUsed", base.DEC)
Generation = ProtoField.uint64("tetrys.generation", "Generation", base.DEC)
PayloadSize = ProtoField.uint16("tetrys.payload_size", "PayloadSize", base.DEC)
Payload = ProtoField.bytes("tetrys.payload", "Payload", base.COLON)
NbIds = ProtoField.uint8("tetrys.nb_ids", "NumberIDs", base.DEC)
FirstSourceId = ProtoField.uint8("tetrys.first_source_id", "FirstSourceID", base.DEC)
IdBitVStart = ProtoField.uint8("tetrys.id_bit_vector_start", "IDBitVectorStart", base.DEC)
IdBitVEnd = ProtoField.uint8("tetrys.id_bit_vector_end", "IDBitVectorEnd", base.DEC)
AckStart = ProtoField.uint8("tetrys.ack_start", "AckStart", base.DEC)
AckEnd = ProtoField.uint8("tetrys.ack_end", "AckEnd", base.DEC)
CurrentWindowCycle = ProtoField.uint8("tetrys.current_window_cycle", "CurrentWindowCycle", base.DEC)


tetrys_protocol.fields = {MessageType, MessageId, SmallestGenerationUsed, Generation, PayloadSize, Payload, NbIds, FirstSourceId, IdBitVStart, IdBitVEnd, AckStart, AckEnd, CurrentWindowCycle}

function tetrys_protocol.dissector(buffer, pinfo, tree)
  length = buffer:len()
  if length == 0 then return end

  pinfo.cols.protocol = tetrys_protocol.name

  local subtree = tree:add(tetrys_protocol, buffer(), "Tetrys Protocol Data")
  
  
  subtree:add(MessageType, buffer(0,1))
  
  
  if buffer(0,1):int() == 0 then
    subtree:add(MessageId, buffer(1,2))
    subtree:add(CurrentWindowCycle, buffer(3,1))
    subtree:add(Generation, buffer(4,8))
    subtree:add(PayloadSize, buffer(12,2))
    subtree:add(Payload, buffer(14))
  
  end
  
  if buffer(0,1):int() == 1 then
    subtree:add(MessageId, buffer(1,2))
    subtree:add(CurrentWindowCycle, buffer(3,1))
    local subsubtree = subtree:add(tetrys_protocol, buffer(), "Encoding Vector")
    subsubtree:add(SmallestGenerationUsed, buffer(4,8))
    subsubtree:add(Generation, buffer(12,8))
    subtree:add(PayloadSize, buffer(20,2))
    subtree:add(Payload, buffer(22))
  
  end
  
  if buffer(0,1):int() == 2 then
    subtree:add(Generation, buffer(1,8))
  
  end
  
end

local udp_port = DissectorTable.get("udp.port")
udp_port:add(1337, tetrys_protocol)
local tcp_port = DissectorTable.get("tcp.port")
tcp_port:add(1338, tetrys_protocol)
